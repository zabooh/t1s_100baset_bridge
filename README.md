# t1s_100baset_bridge

A **generic 10BASE-T1S ↔ 100BASE-T Layer-2 bridge** firmware for the
ATSAME54P20A. It bridges an arbitrary 10BASE-T1S segment onto ordinary Fast
Ethernet, so any device on the T1S side becomes reachable — and reachable
*from* — a normal IP network, exactly as if it were plugged into a regular
Ethernet switch. It ships with on-board diagnostics for the bridge itself
(packet mirroring, register access, PLCA control, a raw-Ethernet loopback
test, an `iperf` throughput tester) plus **persistent network/PLCA
configuration** on an Emulated EEPROM (§6.2).

Unlike a USB-to-T1S dongle (which only gives the *host PC's own software* a
window onto the bus), this bridge puts the T1S segment directly onto an IP
network. Plug the 100BASE-T side into a network with DHCP and internet
access, and every node on the T1S segment can talk to a server on the
internet — no PC, no dongle driver, and no software running on a
host machine in between.

---

---

## Contents

- [1. What this firmware is for](#1-what-this-firmware-is-for)
- [2. Features](#2-features)
  - [Bridging (core function)](#bridging-core-function)
  - [Diagnostics and bus analysis](#diagnostics-and-bus-analysis)
  - [LAN8651 register and PHY access](#lan8651-register-and-phy-access-lan865x_diagc-portable-module)
  - [Persistent configuration (`env` group)](#persistent-configuration-env-group-emulated-eeprom)
  - [Throughput testing](#throughput-testing)
  - [Host-side tooling and build system](#host-side-tooling-and-build-system)
- [3. Hardware setup](#3-hardware-setup)
  - [Bridge board: bill of materials](#bridge-board-bill-of-materials)
  - [How `eth0` (LAN865x) is wired](#how-eth0-lan865x-is-wired-from-the-firmware-config)
  - [Network and addressing (default)](#network-and-addressing-default)
  - [Host PC: giving the `eth1` adapter a static address](#host-pc-giving-the-eth1-adapter-a-static-address)
  - [Console and cabling](#console-and-cabling)
- [4. Firmware architecture](#4-firmware-architecture)
  - [Block view](#block-view)
  - [The bridge data path](#the-bridge-data-path)
  - [Application state machine (`app.c`)](#application-state-machine-appc)
  - [Persistent config (`env.c`, Emulated EEPROM)](#persistent-config-envc-emulated-eeprom)
  - [Port mirror and SPAN (Wireshark)](#port-mirror-and-span-wireshark)
  - [Throughput testing (iperf)](#throughput-testing-iperf)
  - [CLI commands](#cli-commands)
- [5. Building it yourself](#5-building-it-yourself)
- [6. Changing IP and PLCA configuration](#6-changing-ip-and-plca-configuration)
  - [6.1 Persistent: edit the build config and rebuild](#61-persistent-edit-the-build-config-and-rebuild)
  - [6.2 Persistent via the `env` CLI group (recommended)](#62-persistent-via-the-env-cli-group-recommended)
  - [6.3 Volatile runtime via Harmony stack commands](#63-volatile-runtime-via-harmony-stack-commands--plca_node)
- [7. Port mirror: capturing the T1S bus in Wireshark](#7-port-mirror-capturing-the-t1s-bus-in-wireshark)
  - [7.1 Why a mirror is needed](#71-why-a-mirror-is-needed)
  - [7.2 What gets mirrored (both directions, MAC-filtered)](#72-what-gets-mirrored-both-directions-mac-filtered)
  - [7.3 Using it](#73-using-it)
  - [7.4 Limitations](#74-limitations)
  - [7.5 Where the code lives, and one fragile coupling](#75-where-the-code-lives-and-one-fragile-coupling)
- [8. Throughput testing with iperf](#8-throughput-testing-with-iperf)
  - [Commands](#commands)
  - [`iperf` options](#iperf-options)
  - [Examples](#examples)
- [9. Transmitter test modes](#9-transmitter-test-modes)
  - [The command](#the-command)
  - [Verifying without an oscilloscope](#verifying-without-an-oscilloscope)
  - [Further reading](#further-reading)

---

## 1. What this firmware is for

The board sits between two worlds:

```
   PC / lab network / internet     Bridge (this firmware)            T1S bus
   100BASE-T (RJ45)          ATSAME54P20A + LAN865x + LAN8740     10BASE-T1S (2-wire)
   ┌──────────────┐  100M    ┌───────────────────────────┐  T1S   ┌──────────────┐
   │  Wireshark   │◄────────►│ eth1 (GMAC)   eth0 (LAN865x)│◄──────►│  any T1S     │
   │  ping/server │ .210/.200│   └── MAC bridge (L2) ──┘   │ PLCA   │  node(s)     │
   └──────────────┘          └───────────────────────────┘ node 7  │  e.g. .54... │
                                                                    └──────────────┘
```

It does two jobs:

**a) Transparent L2 bridge.** The two interfaces — `eth0` (the T1S MAC-PHY) and
`eth1` (100BASE-T) — are joined by the Harmony **MAC bridge**, so any traffic
from the 100BASE-T side (ARP, ICMP/ping, mDNS, ordinary IP traffic from a
DHCP/internet-connected network, ...) flows through to any node on the T1S
segment and back, with MAC learning (FDB). From a PC on the 100BASE-T side you
can simply `ping <t1s-node-ip>` and reach it *through* the bridge as if it
were on the local Ethernet — the same holds for a server anywhere on the
internet, if `eth1` is plugged into a network that routes there. The bridge
does **not** forward manually in application code — the Harmony MAC bridge
handles all L2 forwarding in both directions.

**b) T1S bus analyzer / SPAN port.** The firmware can mirror T1S traffic onto
`eth1` so you can capture the two-wire bus in **Wireshark** on the PC —
including replies from the T1S side *and* the bridge's own requests (`mirror`
command). It also has raw frame dump/logging (`ipdump`, `logstat`), a
raw-Ethernet loopback test (`noip_send`), LAN865x register peek/poke
(`lan_read`/`lan_write`), PLCA node-ID control, and per-interface counters.

---

## 2. Features

### Bridging (core function)

- Transparent 10BASE-T1S ↔ 100BASE-T Layer-2 bridge on the ATSAME54P20A:
  `eth0` (LAN8651 T1S MAC-PHY) and `eth1` (GMAC + LAN8740A) joined into one L2
  segment.
- Hardware/stack-level forwarding via the Harmony MAC bridge (2 ports, 17-entry
  FDB, dedicated packet pool) with MAC learning — no manual forwarding in
  application code.
- Bidirectional, protocol-agnostic: ARP, ICMP, mDNS, arbitrary IP traffic pass
  both ways; a T1S node is reachable from the LAN exactly as if plugged into an
  Ethernet switch.
- Real internet reachability for T1S nodes — plug `eth1` into a DHCP/routed
  network and every node on the two-wire segment can talk to outside servers,
  with no PC, dongle driver, or host software in the path.

### Diagnostics and bus analysis

- **Port mirror / SPAN** (`mirror [on|off]`): copies T1S traffic to `eth1` for
  Wireshark, in both directions — RX (endpoint replies addressed to the bridge)
  and TX (the bridge's own outgoing frames, hooked at the LAN865x egress).
  MAC-filtered so the capture is duplicate-free; verbatim L2 frames.
- **Raw-frame test** (`noip_send <n> [gap_ms]`, `noip_stat`): deterministic
  EtherType `0x88B5` frames (fixed 60 bytes, fixed payload, monotonic sequence,
  chosen inter-frame gap) that bypass the TCP/IP stack — the best on-board
  source for reproducible oscilloscope captures and for separating a bus problem
  from an IP-config problem.
- **Packet logging** (`ipdump [0..3]`, `logstat`, `logclear`): deferred
  ring-buffer RX dump per interface, drained ≤10 entries per loop iteration so
  logging never stalls the superloop.
- **Counters and memory** (`stats`, `meminfo`, `dump <addr> <count>`):
  per-interface TX/RX counters that don't touch the SPI path, plus C-runtime and
  TCP/IP heap figures.

### LAN8651 register and PHY access (`lan865x_diag.c`, portable module)

- Generic register peek/poke (`lan_read`, `lan_write`) across all MMS banks,
  address = `MMS<<16 | offset`.
- Read-modify-write with masked verify (`lan_rmw <addr> <mask> <val>`), for
  registers where several control bits share one word (e.g. `T1SPMACTL`).
- IEEE 802.3-2022 §147.5.2 transmitter test modes (`testmode [0..4] [seconds]`):
  output voltage/jitter, droop, PSD mask, high impedance — each set with
  automatic readback verification (`[VERIFY] PASS/FAIL`), decoded mode display,
  and an optional auto-revert timeout so a forgotten test mode can't strand the
  link.
- PLCA node-ID control (`plca_node [id]`, 0 = coordinator), volatile runtime
  path.
- Self-contained and portable: two files, depends only on the LAN865x driver
  plus SYS_CMD/SYS_TIME/SYS_CONSOLE; drops into another project with one init
  and one tasks call.

### Persistent configuration (`env` group, Emulated EEPROM)

- Versioned, CRC-protected record in the last 16 KB of flash: per-interface
  IP/mask/gateway/DNS, both MACs, PLCA node id/count.
- CLI-editable, no rebuild (`showenv`, `setenv`, `saveenv`, `readenv`,
  `resetenv`) — IP and PLCA apply live, MAC at next reset.
- Loaded before the stack: `ENV_Init()` runs ahead of `TCPIP_STACK_Init()`, so a
  persisted MAC is in effect when interfaces bind.
- Per-board unique MAC derived from the SAME54's 128-bit serial number, seeded
  on first boot from the compiled defaults — one firmware image, distinct
  boards.
- Survives reflash (emulated EEPROM lies outside the hex image).

### Throughput testing

- Built-in iperf2-compatible tester (`iperf`, `iperfk`, `iperfi`, `iperfs`):
  TCP/UDP, server or client, bandwidth/duration/datagram-size/MSS/ToS options —
  measures end-to-end throughput across the bridge path (PC → `eth1` → MAC
  bridge → `eth0` → endpoint).

### Host-side tooling and build system

- `build.bat` / `flash.bat` / `setup.bat`: headless MPLAB-X build wrapper plus a
  pyOCD flasher with probe auto-detect — no MPLAB X needed just to program the
  board.
- Committed HEX under `release/` — a fresh clone can flash without building.
- Build summary after every build (flash/RAM usage, heap, interrupt handlers).
- `cli.py` — send CLI commands and collect answers over the EDBG COM port
  (115200 8N1).
- Automated test scripts with non-zero exit on failure:
  - `test_lan8651.py` — verifies all four test modes on three independent levels
    (register readback, endpoint traffic stops, traffic resumes), using the
    endpoint's own periodic frames counted by `tshark` as the oracle; enforces
    PLCA-coordinator role and always restores normal operation.
  - `test_mirror.py` — guards the fragile MCC-generated TX-mirror patch (mirror
    off = 0 frames, on = both directions).
  - `smoketest.py` — bridge reachability, L2 forwarding to the endpoint, and
    console liveness.

---

## 3. Hardware setup

The bridge node is built from a Microchip SAM E54 Curiosity board with one
MikroElektronika Click add-on. Whatever device(s) sit on the T1S side is a
*separate* device (the thing you reach through the bridge); it is not part of
the bridge board, and this firmware makes no assumption about what chip or
product it is.

![The assembled bridge board: the SAM E54 Curiosity Ultra host (red board) with the green Two-Wire ETH Click (10BASE-T1S MAC-PHY, plugged into the "X32" header, top left) for eth0, and the LAN8740A PHY Daughter Board (AC320004-3, bottom left, with the RJ45 jack) for eth1 — note it is a separate plug-in module, not part of the Curiosity board itself.](boards.jpg)

### Bridge board: bill of materials

| Function | Board | Microchip order number |
|---|---|---|
| **MCU host** (Cortex-M4F, runs this firmware) | SAM E54 Curiosity (Ultra) board | **DM320210** |
| **100BASE-T PHY** for `eth1` (GMAC ↔ RMII, plugs into the board's PHY daughter-card header) | Microchip **LAN8740A PHY Daughter Board** | **AC320004-3** |
| **10BASE-T1S MAC-PHY** for `eth0` (SPI ↔ two-wire bus) | MikroElektronika **Two-Wire ETH Click** (LAN8651) | **MIKROE-5543** |

> **`eth1` (100BASE-T) needs the LAN8740A PHY Daughter Board, order number
> `AC320004-3`, plugged into the SAM E54 Curiosity (Ultra)'s PHY expansion
> header — the LAN8740A is not soldered onto the Curiosity board itself.**
> This is a compiled-in driver dependency, not just a config value: MCC
> generates a chip-specific Ethernet PHY driver
> (`DRV_ETHPHY_LAN8740`/`drv_extphy_lan8740.c`); the `DRV_LAN8740_PHY_*`
> macros in `configuration.h` only tune *that* driver (RMII mode, PHY
> address **0**, link-negotiation timeouts) — they don't make it work with a
> different PHY chip.
>
> **Do not confuse this with Microchip's more commonly bundled PHY daughter
> board for this same header, the KSZ8061 (`AC320004-6`)** — same connector,
> different chip, needs a different Harmony PHY driver component selected in
> MCC and regenerated, not a macro edit. The `AC320004-x` series covers
> several interchangeable-connector PHY/switch daughter boards (`-4` =
> LAN9303, `-5` = KSZ8041, `-6` = KSZ8061, `-7` = KSZ8863); make sure `-3`
> (LAN8740A) is the one that's fitted.
>
> **`eth0` (10BASE-T1S)** uses the **LAN8651** MAC-PHY on the Two-Wire ETH
> Click, driven by `DRV_LAN865X` over SERCOM SPI. The SPI pin assignment in the
> firmware is fixed (CS=PC15, INT=PC14, see below) and the Click's wiring must
> land on those pins.

### How `eth0` (LAN865x) is wired (from the firmware config)

| Signal | SAM E54 pin | Notes |
|---|---|---|
| SPI instance | SERCOM **SPI driver instance 0** | `DRV_LAN865X_SPI_DRIVER_INSTANCE_IDX0 = 0` |
| SPI clock | **15 MHz** | `DRV_LAN865X_SPI_FREQ_IDX0 = 15000000` |
| Chip select (CS) | **PC15** | `DRV_LAN865X_SPI_CS_IDX0 = SYS_PORT_PIN_PC15` |
| Interrupt (INT) | **PC14** | `DRV_LAN865X_INTERRUPT_PIN_IDX0 = SYS_PORT_PIN_PC14` |

### Network and addressing (default)

| Interface | Role | IP | Mask | PLCA |
|---|---|---|---|---|
| `eth0` | T1S (LAN865x) | **192.168.0.200** | /24 | node id **7** (see `configuration.h`), node count **8** |
| `eth1` | 100BASE-T (GMAC) | **192.168.0.210** | /24 | — |
| T1S node (example) | *any device* | e.g. `192.168.0.54` | /24 | follower |

Both bridge interfaces share one `192.168.0.0/24` subnet (gateway
`192.168.0.1`) — the MAC bridge makes that a single L2 segment. Put the PC's
RJ45 adapter on the same subnet, on an address **other than** `.200`/`.210`/
whatever address(es) the T1S node(s) use (e.g. `192.168.0.220`). Instead of a
PC, `eth1` can equally be plugged into a switch/router that hands out DHCP
and routes to the internet — the T1S node(s) then get real IP connectivity,
including to servers outside the local network.

> **PLCA coordinator.** If the T1S side is meant to run with this board as
> coordinator, set the node id to **0** (`plca_node 0` at runtime, or
> `DRV_LAN865X_PLCA_NODE_ID_IDX0` in `configuration.h` for a persistent
> change) — see [§6](#6-changing-ip-and-plca-configuration).

### Host PC: giving the `eth1` adapter a static address

Both bridge interfaces are compiled as `TCPIP_NETWORK_CONFIG_IP_STATIC`
(`configuration.h`), so they hold `.200`/`.210` for as long as the board runs and
never release them. There is no DHCP server on this segment either — the PC's
adapter therefore needs a **manually assigned** address in `192.168.0.0/24` that
is not already taken. The examples in this README use `192.168.0.220`; any free
address works.

#### Windows

The GUI route (*Network Connections → adapter → Properties → IPv4 → Properties*)
works, but two failure modes look alike and are worth telling apart. Run
`ipconfig` and look at the adapter's block:

| What `ipconfig` shows | Meaning | Fix |
|---|---|---|
| Only `Autoconfiguration IPv4 Address` `169.254.x.x`, **no** `IPv4 Address` line | The static settings were never committed — the interface is still in DHCP mode and no server answered | In the GUI, `OK` has to be confirmed in **both** dialogs (the IPv4 properties *and* the adapter properties window); then re-run `ipconfig` |
| The static address, followed by `(Duplicate)` | Address conflict — Windows' ARP probe found the address in use, disabled it, and fell back to APIPA | Pick a free address; `.200` and `.210` belong to the bridge, and because it is an **L2 bridge** the PC sees *both* on the same segment |

Skipping the dialogs avoids the first case entirely. In an **administrator**
console:

```bat
netsh interface ip set address name="Ethernet 8" static 192.168.0.220 255.255.255.0
netsh interface ip show addresses name="Ethernet 8"
```

Replace `Ethernet 8` with the adapter's name as listed by
`netsh interface ip show config`. A default gateway is not needed for a direct
board-to-PC link. The second command is the check: the address must appear as a
real `IP Address`, not as an autoconfiguration one.

This is **persistent**. `netsh` writes the same registry values the GUI does
(`HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{GUID}`,
with `EnableDHCP = 0`), so it survives reboots, driver restarts and unplugging
the adapter. One caveat for **USB Ethernet adapters**: the setting is bound to
the adapter *instance* (its GUID), not to the friendly name. The same adapter
keeps its instance across USB ports, but plugging in a *different* USB NIC
creates a new instance (`Ethernet 9`, …) that starts out on DHCP — if a
`169.254.x.x` address reappears after a hardware change, check
`netsh interface ip show config` for which interface is actually being
configured.

#### Linux

```sh
sudo ip addr add 192.168.0.220/24 dev eth0     # adapter name from `ip link`
```

#### Verifying the link

Two pings, each with its own diagnostic meaning:

```sh
ping 192.168.0.210     # eth1 (GMAC) answers -> cable and 100BASE-T link are up
ping 192.168.0.200     # eth0 (LAN865x) answers -> the bridge really forwards to the T1S side
```

If the first succeeds and the second does not, the problem is no longer on the
host side: check `stats` and the PLCA configuration
([§6](#6-changing-ip-and-plca-configuration)).

### Console and cabling

1. **Debugger + console:** one USB cable from the PC to the SAM E54 Curiosity
   board's **embedded-debugger** USB port. This is both the programmer
   (PKOB/EDBG) and the virtual COM port for the CLI (**115200 8N1**).
2. **100BASE-T:** the RJ45 on the **LAN8740A PHY Daughter Board** (`AC320004-3`,
   plugged into the Curiosity board's PHY header) ↔ the PC's Ethernet adapter
   (the one set to `192.168.0.220`).
3. **T1S:** the two-wire bus from the LAN865x Click to whatever node(s) sit
   on the T1S segment.

---

## 4. Firmware architecture

Built on **MPLAB Harmony 3** for the ATSAME54P20A. Single-threaded cooperative
superloop (`SYS_Tasks()` in `main.c`); no RTOS, no threads, no locks.

### Block view

```
                       ┌──────────────────────────────────────────────┐
   serial CLI ───────► │ SYS_CMD console: "Test" + "env" groups       │
   (EDBG COM)          │   (app.c / env.c / lan865x_diag / mirror)    │
                       ├──────────────────────────────────────────────┤
   T1S bus  ◄──────────┤ eth0: DRV_LAN865X ┐                          │
                       │                   ├─ TCPIP MAC bridge (L2) ─┐ │
   100BASE-T ◄─────────┤ eth1: GMAC+LAN8740┘   + Harmony TCP/IP stack │ │
                       ├──────────────────────────────────────────────┤
                       │ Emulated EEPROM (last 16 KB flash) — env.c   │
                       └──────────────────────────────────────────────┘
                                         packet handlers (app.c):
                                         pktEth0Handler / pktEth1Handler
```

### The bridge data path

- `TCPIP_STACK_USE_MAC_BRIDGE` is enabled with **2 ports** (`eth0`, `eth1`), a
  17-entry FDB, and a dedicated packet pool. **The MAC bridge does all L2
  forwarding** between T1S and 100BASE-T — there is no manual forwarding code in
  the application.
- `pktEth0Handler` / `pktEth1Handler` are **non-consuming observers**: they run
  per RX frame for logging/mirroring, then return `false` so the frame proceeds
  to normal stack + bridge processing.

### Application state machine (`app.c`)

> **LAN865x register access, the transmitter test modes and PLCA are not in
> `app.c`.** They live in a self-contained module,
> [`firmware/src/lan865x_diag.c`](firmware/src/lan865x_diag.c) / [`.h`](firmware/src/lan865x_diag.h),
> which depends only on the LAN865x driver and `SYS_CMD`/`SYS_TIME`/`SYS_CONSOLE`
> so it can be lifted into another project unchanged — see
> [§10 of the test-mode guide](LAN8651_TEST_MODES.md#10-porting-this-to-another-project).
> `app.c` contributes exactly two calls to it, listed below.

`APP_Initialize` registers the Telnet auth + a 1 s timer, calls
`Command_Init()` to register the `Test` command group, and
`LAN865X_DIAG_Initialize()` to register the `lan` group. `APP_Tasks` walks
`INIT → WAIT → SERVICE_TASKS → IDLE`: in `SERVICE_TASKS` it registers the two
packet handlers and calls `env_apply()` (push the persisted network config
into the now-running TCP/IP stack); in `IDLE` it (1) calls
`LAN865X_DIAG_Tasks()` to service the async LAN865x register state machine,
and (2) drains the deferred packet-log ring buffer to the
console (≤10 entries/iteration, so logging never stalls the loop). Captured
frame bytes go to a separate circular pool; the ring uses a lock-free
single-producer/consumer pattern (handlers write, `APP_Tasks` reads).

### Persistent config (`env.c`, Emulated EEPROM)

*(Full walkthrough in [§6.2](#62-persistent-via-the-env-cli-group-recommended).)*

A versioned, CRC-protected record (per-interface IP/mask/gateway/DNS, both
MACs, PLCA node id/count) lives in the **Emulated EEPROM** Harmony library
(added via MCC; reserves the last 16 KB of flash). `ENV_Init()` runs at the
very start of `SYS_Initialize()` in `initialization.c` — **before**
`TCPIP_STACK_Init()` — so a persisted or freshly-seeded MAC is already in
effect when the stack binds its interfaces. On a blank/corrupt EEPROM (e.g.
first flash) it seeds itself from the `configuration.h` compiled defaults,
including a per-board MAC derived from the SAME54's serial number.

### Port mirror and SPAN (Wireshark)

*(Full walkthrough and limitations in [§7](#7-port-mirror-capturing-the-t1s-bus-in-wireshark).)*

`mirror 1` turns on two clone paths so a PC capture on `eth1` sees the full T1S
picture, each filtered by the bridge's own `eth0` MAC to stay duplicate-free:
- **RX mirror** (`MIRROR_Eth0Rx()`, called from `pktEth0Handler` in `app.c`):
  frames addressed to the bridge (dst MAC == `eth0`) — the endpoint's replies —
  are cloned to `eth1`.
- **TX mirror** (`mirror_eth0_tx_hook`, called from the LAN865x egress
  `DRV_LAN865X_PacketTx`): frames the bridge itself originates (src MAC ==
  `eth0`) — its `ping`/ARP — are cloned to `eth1`, protocol-independent.
  Because a node never receives its own TX, hooking the egress is the only way
  to see them.

### Throughput testing (iperf)

*(Full option reference and examples in [§8](#8-throughput-testing-with-iperf).)*

`iperf`/`iperfk`/`iperfi`/`iperfs` are the Harmony TCP/IP stack's **built-in**
iperf2-protocol-compatible throughput tester (`library/tcpip/src/iperf.c`,
`TCPIP_STACK_USE_IPERF`) — not something this project added. Useful to measure
end-to-end throughput across the bridge itself (PC → `eth1` → MAC bridge →
`eth0` → endpoint) without any extra PC tooling beyond a compatible `iperf`/
`iperf2` binary.

### CLI commands

Two command groups; type the command name directly (no group prefix needed).

**`Test` group:**

| Command | Description |
|---|---|
| `help` | show this list |
| `timestamp` | firmware build timestamp |
| `ipdump [0..3]` | dump RX frames (0=off, 1=eth0, 2=eth1, 3=both) |
| `stats` | per-interface TX/RX software counters |
| `meminfo` | free memory: C-runtime heap (total + largest free block) **and** TCP/IP heap (free/maxblock/highwater, like `heapinfo`) |
| `dump <addr> <count>` | memory dump (hex) |
| `logstat` / `logclear` | deferred packet-log statistics / clear |

**`noip` group** — raw Ethernet frame test with EtherType 0x88B5, bypassing the
TCP/IP stack entirely, in [`noip_test.c`](firmware/src/noip_test.c). Deterministic
frames (fixed 60-byte length, fixed payload, monotonic sequence number, chosen
inter-frame gap) make this the best on-board source for reproducible oscilloscope
captures and for separating a bus problem from an IP-configuration problem:

| Command | Description |
|---|---|
| `noip_send <n> [gap_ms]` | send `n` frames (1..100) with an optional gap of 0..1000 ms |
| `noip_stat` | TX/RX counters, independent of any protocol state |

**`span` group** — the eth0 → eth1 port mirror, in
[`port_mirror.c`](firmware/src/port_mirror.c) (see [§7.5](#75-where-the-code-lives-and-one-fragile-coupling)):

| Command | Description |
|---|---|
| `mirror [0\|1]` | SPAN: copy T1S (eth0) traffic — RX **and** the bridge's own TX — to eth1 for Wireshark |

**`lan` group** — LAN865x registers, transmitter test modes and PLCA. Lives in the
self-contained [`lan865x_diag.c`](firmware/src/lan865x_diag.c) module rather than in
`app.c`, so it can be reused in another project
(see [§10 of the test-mode guide](LAN8651_TEST_MODES.md#10-porting-this-to-another-project)):

| Command | Description |
|---|---|
| `lanhelp` | list these commands with a short usage reminder |
| `plca_node [id]` | get/set PLCA node id (0 = coordinator); no arg = show current — **volatile**, see [§6.3](#63-volatile-runtime-via-harmony-stack-commands--plca_node) |
| `lan_read <addr>` / `lan_write <addr> <val>` | LAN865x register access (hex) |
| `lan_rmw <addr> <mask> <val>` | read-modify-write a single register, then verify it: `new = (old & ~mask) \| val`. For registers where several control bits share one word, e.g. `T1SPMACTL` |
| `testmode [0..4] [seconds]` | select an IEEE 802.3-2022 §147.5.2 transmitter test mode, verified by readback; no argument shows the current mode. The optional timeout reverts to normal operation on its own — see [§9](#9-transmitter-test-modes) |

**`env` group** — persistent config on the Emulated EEPROM (see [§6.2](#62-persistent-via-the-env-cli-group-recommended)):

| Command | Description |
|---|---|
| `showenv` | show the current config: per-interface IP/mask/gw/dns, MAC, PLCA id/count |
| `setenv <key> <val>` | edit the RAM shadow — keys: `ip0/mask0/gw0/dns0`, `ip1/…`, `mac0`/`mac1`, `plca_id`/`plca_cnt` |
| `saveenv` | persist to EEPROM **and** apply (IP/PLCA live; MAC at next reset) |
| `readenv` | reload from EEPROM and apply (discard unsaved edits) |
| `resetenv` | restore the compiled defaults, persist and apply |

**`iperf` group** — Harmony's built-in throughput tester (see [§8](#8-throughput-testing-with-iperf)):

| Command | Description |
|---|---|
| `iperf [options]` | start a throughput test session (server or client) |
| `iperfk` | stop the running session |
| `iperfi <address>` | bind the test to a specific local interface |
| `iperfs <tx\|rx> <bytes>` | set the TX/RX buffer size |

Harmony stack commands (`netinfo`, `bridge`, `ping`, `setip`, `setgw`, etc.) are
also available.

---

## 5. Building it yourself

This is a plain **MPLAB X** project (no CMake/Ninja) with a thin shell wrapper
around MPLAB X's own build, plus a **pyOCD**-based flash tool (no MDB/MPLAB X
needed just to program the board).

### 5.1 Tool prerequisites (per machine)

| Requirement | Notes |
|---|---|
| **MPLAB X IDE** | needed once to generate the project's build files (see 5.2) and for the SAME54_DFP device pack |
| **MPLAB XC32** | this firmware was built with XC32 v4.60, under `C:\Program Files\Microchip\xc32\` |
| **Python 3.9+** | `pyserial` for `cli.py`/`smoketest.py`, `pyocd` for `flash.bat` (installed by setup) |
| **Terminal** | the board's EDBG virtual COM port, 115200 8N1 |

### 5.2 One-time setup after cloning

```bat
git clone https://github.com/zabooh/t1s_100baset_bridge.git
cd t1s_100baset_bridge
setup.bat
```

`setup.bat` runs four independent steps (a failure in one is reported but does
not abort the rest): Python deps (`pyserial`), XC32 version selection
(`setup_compiler.py`), pyOCD + probe/pack check (`install.bat --install`), and
the SAME54_DFP VS Code debug fix (`setup_debug.py`).

**Additional one-time step, MPLAB X itself:** a fresh checkout has no
`nbproject/Makefile-*.mk` fragments — MPLAB X generates those the first time
the project is opened and built in the IDE, and `build.bat` drives that same
generated Makefile headlessly afterwards. So once per machine:

1. Open MPLAB X IDE → *File → Open Project* → `firmware/T1S_100BaseT_Bridge.X`.
2. Right-click the project → *Clean and Build* (once).

After that, `build.bat` works without reopening the IDE.

### 5.3 Build and flash

```bat
build.bat            :: incremental build  (build.bat rebuild = clean build, build.bat help)
flash.bat             :: program the board via pyOCD and release it from reset
```

`build.bat`:
1. drives MPLAB X's generated Makefile (`make CONF=default TYPE_IMAGE=PRODUCTION`)
   with the XC32 bin dir from `setup_compiler.config`,
2. copies the resulting HEX into a tracked **`release/T1S_100BaseT_Bridge.hex`**,
3. prints a flash/RAM/heap/IRQ **build summary** (`build_summary.py`).

Because the HEX is committed under `release/`, a **fresh clone can flash
without building**:

```bat
flash.bat             :: flashes release/T1S_100BaseT_Bridge.hex
```

`flash.bat` (via `flash_same54.py`) auto-detects the connected EDBG/nEDBG
probe and the locally installed Microchip SAME54_DFP pack, then flashes over
SWD with **pyOCD** and resets the board so it starts immediately.
`flash.bat --list` lists connected probes; `install.bat` checks/installs
pyOCD and reports whether a probe and the device pack are visible.

### 5.4 Smoke test

After flashing, verify the bridge end to end from the PC:

```bat
python smoketest.py --bridge 192.168.0.210 --endpoint 192.168.0.54 --com COM8
```

Checks bridge reachability, L2 forwarding to the endpoint (ping through the
bridge), and — if `--com` is given — that the on-board console answers
`stats`. Exits non-zero if any check fails.

`cli.py` sends ad-hoc CLI commands over the EDBG COM port, e.g.
`python cli.py --port COM8 "stats" "plca_node"`.

### First bring-up checklist

1. `setup.bat` → open+build once in MPLAB X (5.2) → `build.bat` → `flash.bat`.
2. Open the EDBG COM port at 115200 8N1; you should see the build banner.
3. `stats` — confirm `eth0`/`eth1` exist and counters move.
4. `plca_node` — reports the configured node id.
5. From the PC (on `192.168.0.220`): `ping 192.168.0.54` → 0% loss (bridge
   works). Or run `python smoketest.py`.

---

## 6. Changing IP and PLCA configuration

The IP addresses (`eth0` = 192.168.0.200, `eth1` = 192.168.0.210) and the PLCA
parameters can be changed two ways:

- **Persistent, no rebuild — the `env` command group (§6.2).** Backed by the
  Emulated EEPROM (see [§4](#4-firmware-architecture)); survives reset/power-cycle.
  This is the recommended way to change a board's config.
- **Persistent, requires rebuild — edit `configuration.h` (§6.1).** Only
  matters for the *compiled-in* defaults that `env` seeds a blank/freshly
  flashed EEPROM from (`resetenv` also restores these).

### 6.1 Persistent: edit the build config and rebuild

All defaults live in **`firmware/src/config/default/configuration.h`** (an
MCC-generated file). Edit the macros, then rebuild + reflash in MPLAB X.

| Setting | Macro (`configuration.h`) | Default |
|---|---|---|
| eth0 (T1S) IP | `TCPIP_NETWORK_DEFAULT_IP_ADDRESS_IDX0` | `"192.168.0.200"` |
| eth0 subnet mask | `TCPIP_NETWORK_DEFAULT_IP_MASK_IDX0` | `"255.255.255.0"` |
| eth0 gateway | `TCPIP_NETWORK_DEFAULT_GATEWAY_IDX0` | `"192.168.0.1"` |
| eth0 MAC | `TCPIP_NETWORK_DEFAULT_MAC_ADDR_IDX0` | `"00:04:25:01:02:03"` (fallback only — `env` derives the real per-board MAC from the SAME54 serial number, see §4) |
| eth1 (100BASE-T) IP | `TCPIP_NETWORK_DEFAULT_IP_ADDRESS_IDX1` | `"192.168.0.210"` |
| eth1 subnet mask | `TCPIP_NETWORK_DEFAULT_IP_MASK_IDX1` | `"255.255.255.0"` |
| eth1 gateway | `TCPIP_NETWORK_DEFAULT_GATEWAY_IDX1` | `"192.168.0.1"` |
| eth1 MAC | `TCPIP_NETWORK_DEFAULT_MAC_ADDR_IDX1` | `"00:04:25:01:02:04"` (fallback only, see above) |
| PLCA node id | `DRV_LAN865X_PLCA_NODE_ID_IDX0` | `7` |
| PLCA node count | `DRV_LAN865X_PLCA_NODE_COUNT_IDX0` | `8` |

**These macros are only the fallback defaults — a stored `env` record wins.**
`initialization.c` seeds the LAN865x driver from the PLCA macro at driver init,
but once the stack is up, `env_apply()` ([`app.c`](firmware/src/app.c) →
[`env.c`](firmware/src/env.c)) pushes the *persisted* configuration into the
stack and calls `APP_ApplyPlca(env_plca_id(), env_plca_cnt())` on top of it. The
macros are consulted only when the EEPROM record is missing, blank or fails its
CRC check (e.g. on a virgin board or after `resetenv`).

Two consequences worth knowing before you edit anything here:

- Flashing with `flash.bat` uses pyOCD's default **sector** erase, and the
  emulated EEPROM lives in the last 16 KB of flash (`0xFC000`), outside the hex
  image. A saved configuration therefore **survives a rebuild and reflash** —
  which is convenient, but it also means an edited macro can appear to have no
  effect at all.
- Run **`showenv`** to see what is actually in effect. To make macro changes take
  hold, either `resetenv` (drops the stored record) or set the value directly
  with `setenv` + `saveenv` as described in §6.2 — the latter needs no rebuild
  and is the faster route for a single value.

> **Keep both interfaces on the same subnet as the endpoint and the PC**, since
> the MAC bridge makes them one L2 segment. If this board should be the PLCA
> coordinator, set `DRV_LAN865X_PLCA_NODE_ID_IDX0` to `0` — and remember that a
> stored `env` record overrides it, so check `showenv` afterwards. Note that
> `plca_node 0` on the console is **volatile**: it is lost on the next reset,
> including the reset that `flash.bat` performs after programming.
>
> **MCC note:** `configuration.h` is generated by MCC. A plain text edit +
> rebuild is fully supported. Only if you *re-run MCC code generation* will it
> be overwritten — in that case make the change in the MCC project (TCP/IP
> network config / LAN865x PLCA) instead.

### 6.2 Persistent via the `env` CLI group (recommended)

The `env` group (`env.c`) keeps a versioned, CRC-protected copy of the
per-interface IP/mask/gateway/DNS, both MACs, and the PLCA node id/count in
the **Emulated EEPROM** — the last 16 KB of flash, reserved by the Emulated
EEPROM library added via MCC. `ENV_Init()` runs at the very start of
`SYS_Initialize()` (before `TCPIP_STACK_Init()`), so a persisted MAC is
already in effect when the stack binds its interfaces.

On first boot (blank/corrupt EEPROM, e.g. right after flashing this firmware
for the first time) `ENV_Init()` seeds the record from the `configuration.h`
defaults (§6.1) — including a **per-board MAC** derived from the SAME54's
128-bit serial number (see [§4](#persistent-config-envc-emulated-eeprom)), so
every board is unique from one firmware image.

#### `setenv` keys

| Key(s) | Value format | Field |
|---|---|---|
| `ip0`, `mask0`, `gw0`, `dns0` | dotted-quad, e.g. `192.168.0.201` | eth0 (T1S) IP/mask/gateway/DNS |
| `ip1`, `mask1`, `gw1`, `dns1` | dotted-quad | eth1 (100BASE-T) IP/mask/gateway/DNS |
| `mac0`, `mac1` | `XX:XX:XX:XX:XX:XX` | eth0/eth1 MAC — applies **after the next reset** |
| `plca_id` | `0`..`254` | PLCA node id (0 = coordinator) |
| `plca_cnt` | `1`..`255` | PLCA node count |

#### Worked example

Change eth0's DNS server, persist it, and prove it survives a reboot (this
transcript is a real console session, via `python cli.py --port COM8 "..."`):

```text
> showenv
env (RAM shadow):
  eth0  ip 192.168.0.200  mask 255.255.255.0  gw 192.168.0.1  dns 8.8.8.8
  eth1  ip 192.168.0.210  mask 255.255.255.0  gw 192.168.0.1  dns 8.8.8.8
  eth0  mac 00:04:25:CA:CE:D9
  eth1  mac 00:04:25:CA:CE:DA  (applied at boot)
  plca  id 7  count 8  (eth0/T1S)

> setenv dns0 1.1.1.1
setenv: dns0 = 1.1.1.1 (RAM only; 'saveenv' to persist)

> saveenv
saveenv: persisted to EEPROM and applied (an IP change drops the current connection).

> showenv
  eth0  ip 192.168.0.200  mask 255.255.255.0  gw 192.168.0.1  dns 1.1.1.1   <- updated

--- board reset (power-cycle, or e.g. "python -m pyocd reset ...") ---

> showenv
  eth0  ip 192.168.0.200  mask 255.255.255.0  gw 192.168.0.1  dns 1.1.1.1   <- survived the reset
```

`dns0` is still `1.1.1.1` after the reset — the EEPROM record, not the
`configuration.h` compiled default (`8.8.8.8`), is what `ENV_Init()` loaded
this time. To go back to the compiled defaults: `resetenv` (also persists and
applies immediately, no reset needed):

```text
> resetenv
resetenv: restored compiled defaults, persisted and applied.

> showenv
  eth0  ip 192.168.0.200  mask 255.255.255.0  gw 192.168.0.1  dns 8.8.8.8
  plca  id 7  count 8  (eth0/T1S)
```

A **MAC** change follows the same `setenv mac0 XX:XX:XX:XX:XX:XX` + `saveenv`
pattern, but — unlike IP/PLCA — only takes effect after the *next* reset,
since the TCP/IP stack reads the MAC once, at `TCPIP_STACK_Init()`.

#### PLCA node id: which command writes the PHY, and how to prove it

A **PLCA node id/count** change persists *and* takes effect without a reset —
but the two halves are done by two different commands, and it is worth knowing
which is which:

| Command | Effect |
|---|---|
| `setenv plca_id 0` | **RAM only.** Updates the shadow record and says so (`RAM only; 'saveenv' to persist`). No register is written, the PHY is untouched. |
| `saveenv` | Writes the EEPROM **and** calls `env_apply()`, which calls `APP_ApplyPlca()` → a `PLCA_CTRL1` write to `0x0004CA02` (`NODE_CNT` in bits 15:8, `NODE_ID` in 7:0). This is the step that reaches the LAN865x. |

So `saveenv` is what configures the PHY. On success the console prints:

```text
> setenv plca_id 0
setenv: plca_id = 0 (RAM only; 'saveenv' to persist)

> saveenv
[PLCA] node ID set to 0 (NODE_CNT=8, reg=0x00000800)
saveenv: persisted to EEPROM and applied (an IP change drops the current connection).
```

**Watch for the skip case.** `APP_ApplyPlca()` uses the same single-slot LAN
register state machine as `lan_read`/`lan_write` (one operation at a time,
serviced only in `APP_STATE_IDLE`, 200 ms timeout). If that machine is busy it
returns without writing anything:

```text
[PLCA] LAN busy - apply skipped (retry when idle)
```

The EEPROM then holds the new node id while the PHY still runs the old one —
persisted and live state diverge, and the mismatch silently repairs itself on
the next reset, which makes it easy to misread. Simply repeat `saveenv` once the
board is idle.

**Verify with a register read, not with `plca_node`.** The bare `plca_node`
query prints the driver-side `s_plca_node_id`, which is assigned *before* the
register write is carried out — it reports the intent, not the state of the PHY.
The authoritative check is:

```text
> lan_read 0x0004CA02
```

| Value | Meaning |
|---|---|
| `0x00000800` | `NODE_CNT` = 8, `NODE_ID` = 0 → this board is the PLCA coordinator |
| `0x00000807` | still node id 7 → the write did not get through; repeat `saveenv` |

Same principle as the transmitter test modes: the readback decides, not the
confirmation message. Do not poll this during a throughput test — register
access shares the SPI/TC6 service path with the data path.

The `Test` group's own bare `plca_node <id>` (§6.3) is a separate, quicker path
for trying a node id **without** touching the EEPROM at all — but it is
volatile, and is lost on the next reset, including the one `flash.bat` performs
after programming.

### 6.3 Volatile runtime via Harmony stack commands / `plca_node`

The Harmony TCP/IP stack commands and `plca_node` (in the `Test` group) let
you try a value without touching the EEPROM at all. Run `netinfo` first to
see the exact interface names (`eth0`/`eth1`).

```text
netinfo                                   # show both interfaces, IPs, MACs, status
setip  eth0 192.168.0.190 255.255.255.0   # set eth0 IPv4 address + mask
setgw  eth0 192.168.0.1                    # set eth0 gateway
setip  eth1 192.168.0.191 255.255.255.0   # set eth1 IPv4 address + mask
plca_node 0                                # set PLCA node id (writes PLCA_CTRL1)
plca_node                                  # (no arg) read back the current node id
```

> ⚠️ **These specific commands are volatile.** Anything set with `setip`/
> `setgw`/bare `plca_node <id>` is lost on the next reset or power-cycle — the
> board boots back to whatever `env` has persisted (§6.2), which itself
> defaults to the `configuration.h` values (§6.1) on a blank EEPROM. Use these
> only to try a value before persisting it with `setenv`/`saveenv`.

---

## 7. Port mirror: capturing the T1S bus in Wireshark

The `mirror` command turns the bridge into a SPAN/monitor port: it copies T1S
(`eth0`) traffic onto `eth1` so a PC running Wireshark on its Fast-Ethernet
adapter can see the two-wire bus.

### 7.1 Why a mirror is needed

Two things are otherwise invisible to a PC capture on `eth1`:

1. **The endpoint's replies** arrive on `eth0` and — because they are
   addressed to the bridge itself — are delivered *locally* by the MAC bridge,
   not forwarded onto `eth1`. So a plain `eth1` capture never shows them.
2. **The firmware's own requests** (a `ping`, ARP, ...) are sent *out* of
   `eth0` by the bridge. A node never receives its own transmissions, so no RX
   packet handler ever sees them either.

The mirror reconstructs **both** directions onto `eth1`, protocol-independent,
so a firmware-originated `ping` to the endpoint is visible in full (request
*and* reply).

### 7.2 What gets mirrored (both directions, MAC-filtered)

Both directions are cloned to `eth1`, but each is **filtered by the bridge's
own `eth0` MAC** so the capture is **duplicate-free** — frames the MAC bridge
merely *forwards* between the PC and the bus (which the PC already sees
natively on `eth1`) are not mirrored.

| Path | Hook (`port_mirror.c`) | Filter | What it captures |
|---|---|---|---|
| **RX mirror** | `MIRROR_Eth0Rx()`, from `pktEth0Handler` in `app.c` | dst MAC **==** `eth0` MAC | frames addressed to the bridge itself — the endpoint's unicast replies to the firmware |
| **TX mirror** | `mirror_eth0_tx_hook()`, from `DRV_LAN865X_PacketTx` (the single `eth0` egress) | src MAC **==** `eth0` MAC | frames the bridge itself originates — the firmware's `ping`/ARP, regardless of protocol |

Why this is duplicate-free, given the bridge does transparent L2 forwarding
(source/destination MACs are preserved):

- A PC→endpoint frame forwarded onto `eth0` keeps the **PC's** src MAC → TX
  filter skips it (the PC already sent it).
- An endpoint→PC frame received on `eth0` carries the **PC's** dst MAC → RX
  filter skips it (the bridge forwards it to `eth1` natively).
- Broadcast/multicast received on `eth0` is forwarded to `eth1` by the bridge,
  so it is **not** mirrored either; only the bridge's *own* outgoing
  broadcast/multicast (src == `eth0` MAC, e.g. its ARP) is added by the TX
  path.

The original `eth0` frame is never altered — the mirror clones a fresh packet
for `eth1` and leaves the bus frame for normal local/bridge processing.

> **Worked example — `ping` from the firmware to the endpoint (egress on
> `eth0`):** the ICMP echo *request* leaves through `DRV_LAN865X_PacketTx` with
> src == `eth0` MAC → TX-mirrored. The endpoint's echo *reply* arrives on
> `eth0` with dst == `eth0` MAC → RX-mirrored. So Wireshark on `eth1` shows the
> **complete** exchange, both request and reply. (A PC-originated ping passing
> *through* the bridge is not mirrored — the PC already has both halves on
> `eth1`.)

### 7.3 Using it

1. On the PC, start **Wireshark** on the Fast-Ethernet adapter connected to
   the bridge's `eth1` (the LAN8740A PHY Daughter Board's RJ45).
2. On the board CLI: `mirror on` (turn it on). `mirror` with no argument shows
   the current state; `mirror off` turns it off. `1` and `0` are accepted too,
   which is what the test scripts use.
3. Run anything that talks to the endpoint from the bridge itself (e.g. the
   Harmony `ping 192.168.0.54`) and watch the T1S traffic (both directions)
   appear in Wireshark.

```text
mirror on               # eth0(T1S) -> eth1 mirror: ON   (RX + the bridge's own TX)
ping 192.168.0.54        # now visible on eth1 in Wireshark: request + reply
mirror off              # turn it off when done
```

### 7.4 Limitations

- **Exact L2 frames:** both directions clone the real Ethernet frame verbatim
  (header + payload), so MACs and checksums are exactly what went on the wire.
- **The filter relies on transparent bridging:** it assumes the MAC bridge
  does not rewrite source/destination MACs (it doesn't). The reference is the
  `eth0` interface MAC; frames carrying it are treated as the bridge's own.
- **Single-segment copy:** the mirror clones the packet's first data segment.
  The bridge/stack frames involved are single-segment; a hypothetical
  multi-segment frame would be truncated.
- **Broadcast/multicast received on `eth0` is not mirrored** — the bridge
  already forwards it to `eth1`, where the PC sees it natively (see §7.2).
- Mirroring adds one cloned `eth1` transmit per matching frame. It is meant
  for diagnostics — leave it **off** for normal bridging to avoid the extra
  load.
- Mirror state is a runtime toggle (like the §6.2 CLI settings) and defaults
  to **off** on every boot.

### 7.5 Where the code lives, and one fragile coupling

The mirror is a separate module,
[`firmware/src/port_mirror.c`](firmware/src/port_mirror.c) /
[`.h`](firmware/src/port_mirror.h). `app.c` contributes two things only:
`MIRROR_Initialize()` at startup and `MIRROR_Eth0Rx()` from `pktEth0Handler`.

Unlike the test-mode module ([§10 there](LAN8651_TEST_MODES.md#10-porting-this-to-another-project)),
this one is **not** free-standing: it needs the Harmony TCP/IP stack for packet
allocation and `TCPIP_NET_IF` internals, `DRV_GMAC_PacketTx` specifically as the
mirror destination, and the LAN865x driver patched to call into it. It is
reusable in another Harmony two-port bridge — adapt `MIRROR_SRC_IF` /
`MIRROR_DST_IF` and the destination MAC driver — but not in an arbitrary project.

> **The TX half depends on a patch inside MCC-generated code.**
> `DRV_LAN865X_PacketTx()` in
> `src/config/default/driver/lan865x/src/dynamic/drv_lan865x_api.c` declares
> `mirror_eth0_tx_hook` as a local `extern` and calls it. Two consequences: the
> function name is **not** free to change, and **re-running MCC code generation
> removes the call site**. The symptom afterwards is subtle — the capture still
> shows frames *from* the bus but none of the bridge's own, which looks like a
> half-working mirror rather than a missing patch.
>
> **[`test_mirror.py`](test_mirror.py) checks exactly this** and is worth running
> after any regeneration. It pings the T1S node from the board's own console and
> counts the resulting ICMP on `eth1` with `tshark`: that conversation cannot
> reach `eth1` by any route other than the mirror, so mirror-off must yield zero
> frames and mirror-on must yield both directions.
>
> ```text
> python test_mirror.py
> ```

---

## 8. Throughput testing with iperf

`iperf`/`iperfk`/`iperfi`/`iperfs` are **not** a bridge-specific feature —
they're the Harmony TCP/IP stack's own built-in throughput tester
(`library/tcpip/src/iperf.c`, enabled via `TCPIP_STACK_USE_IPERF` in
`configuration.h`), protocol-compatible with the classic `iperf`/`iperf2`
tool, so a PC running `iperf2` can test directly against the board (or vice
versa). Documented here because it's the most useful tool for measuring
**end-to-end throughput across the bridge itself**: PC → `eth1` → the
Harmony MAC bridge → `eth0` → the T1S endpoint, and back — a real-world
number to complement `stats`' packet counters.

Configured in `configuration.h`:

```c
#define TCPIP_STACK_USE_IPERF
#define TCPIP_IPERF_TX_BUFFER_SIZE      4096
#define TCPIP_IPERF_RX_BUFFER_SIZE      4096
#define TCPIP_IPERF_MAX_INSTANCES       1      // one iperf session at a time
#define TCPIP_IPERF_TX_BW_LIMIT         10     // default client TX rate: 10 Mbps
```

### Commands

| Command | Description |
|---|---|
| `iperf [options]` | start a test session (as server or client) |
| `iperfk` | kill/stop the running session |
| `iperfi <address>` | bind the test to a specific local interface address |
| `iperfs <tx\|rx> <bytes>` | set the TX/RX buffer size |

### `iperf` options

| Option | Meaning | Default |
|---|---|---|
| `-s` | run as **server** | — |
| `-c <ip>` | run as **client**, connect to `<ip>` | — |
| `-u` | UDP instead of TCP | TCP |
| `-b <rate>` | target bandwidth for a UDP client (bps; `K`/`M` suffix allowed) — also switches to UDP | 10 Mbps |
| `-t <secs>` | test duration | 10 s |
| `-n <bytes>` | transfer a fixed amount instead of running by time | off |
| `-i <secs>` | report interval | 1 s |
| `-p <port>` | server/target port | 5001 |
| `-l <bytes>` | UDP datagram size | — |
| `-M <bytes>` | TCP MSS | — |
| `-S <tos>` | Type of Service | 0 (best effort) |
| `-x <rate>` | (non-standard) max TCP TX rate, bps | — |

### Examples

On the board, as a server:

```text
iperf -s
```

From the board, as a client against the PC (e.g. `192.168.0.220`), 20 s of
UDP at 50 Mbps:

```text
iperf -c 192.168.0.220 -u -b 50M -t 20
```

Stop a running session:

```text
iperfk
```

Testing the bridge path from the **PC side** works the same way, just with
the roles reversed — e.g. `iperf -s` on the board, then
`iperf2 -c 192.168.0.210` from the PC (through `eth1` → MAC bridge → `eth0`
→ endpoint), or vice versa with the endpoint as the counterpart.

---

## 9. Transmitter test modes

> **Full guide: [`LAN8651_TEST_MODES.md`](LAN8651_TEST_MODES.md)** — what each mode
> qualifies, how to probe and set up the measurement on the bus, what to disconnect
> first, and how the same thing is done with the generic `lan_read`/`lan_write`
> commands versus the `testmode` convenience wrapper. This section is the summary.

The LAN8651 implements the transmitter test modes of **IEEE 802.3-2022
§147.5.2** in hardware. They emit a defined, continuous pattern with no user
traffic, which is what level, jitter, droop and spectrum measurements need.
Selecting one is a plain register write — no firmware change required.

### The command

```text
testmode              # show the current mode, decoded
testmode 1            # enter test mode 1, verified by readback
testmode 1 30         # ... and revert to normal operation after 30 s
testmode 0            # back to normal operation
```

| Mode | Purpose | Instrument |
|---|---|---|
| 0 | normal operation | — |
| 1 | output voltage, timing jitter | oscilloscope |
| 2 | output droop | oscilloscope |
| 3 | PSD mask / transmitter distortion | spectrum analyser |
| 4 | transmitter high impedance | measure the bus *without* this transmitter |

The command writes `T1STSTCTL` (`0x000308FB`, mode in bits 15:13) and then
**reads it back automatically**, reporting `[VERIFY] PASS` or `[VERIFY] FAIL`.
That readback is the actual evidence: `LAN865X Write OK` only says the TC6
transaction completed, not that the register kept the value.

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

Modes 1–4 **take the T1S link down** by design, so the bridged connection is
dead while one is active. The CLI itself is unaffected — it runs over the EDBG
UART, not over T1S, so the way back to normal operation is always available.
Use the optional timeout when in doubt: a forgotten test mode later presents as
a link that will not come up, with nothing in the ordinary log pointing at a
test register.

### Verifying without an oscilloscope

`test_lan8651.py` checks each mode on three independent levels and exits
non-zero if any of them fails:

1. **Register readback** — the firmware's own `[VERIFY]` verdict on `T1STSTCTL`.
2. **Traffic stops** — the endpoint's periodic frames stop arriving on `eth1`.
3. **Traffic resumes** — and come back after reverting.

Level 1 alone only proves the register latched the value; levels 2 and 3 show
the PHY actually changed state. The traffic oracle is whatever the T1S endpoint
transmits by itself — by default its SOME/IP-SD OFFER multicast at 1 Hz —
counted with `tshark` on the `eth1` adapter. Nothing is generated on the host,
so the measurement does not perturb the bus the way polling registers during a
throughput test would.

```text
python test_lan8651.py --port COM8
python test_lan8651.py --port COM8 --modes 1,2 --window 6
python test_lan8651.py --list-interfaces
```

The script **requires this board to be the PLCA coordinator** (node id 0) and
refuses to run otherwise: with an external coordinator the endpoint could keep
transmitting, and "traffic stopped" would no longer say anything about this
board's transmitter. It also always restores normal operation in a `finally`
block and reports whether that succeeded.

Result on this hardware (2026-08-10, SAM E54 Curiosity Ultra + MIKROE-5543,
endpoint at `192.168.0.54`): all four modes PASS on all three levels — 19
checks, exit code 0. What this does **not** establish is whether the emitted
waveform conforms to the standard; that still needs an oscilloscope or spectrum
analyser at the MDI.

### Further reading

**[`LAN8651_TEST_MODES.md`](LAN8651_TEST_MODES.md)** is the full guide: what the four
modes are and what each one qualifies; probing, termination and instrument setup per
mode; what to disconnect on a shared multidrop bus; the generic `lan_read`/`lan_write`
register path versus the `testmode`/`lan_rmw` convenience commands; the `T1SPMACTL` bit
map and PMA loopback; and the verification log for this hardware.
