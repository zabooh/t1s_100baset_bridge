# t1s_100baset_bridge

A **10BASE-T1S ↔ 100BASE-T Layer-2 bridge** firmware for the ATSAME54P20A. It
lets a PC on ordinary Fast Ethernet reach a Microchip **LAN866x 10BASE-T1S
endpoint** that lives on the two-wire T1S bus, and it ships with on-board
diagnostics for the bridge itself (packet mirroring, register access, PLCA
control, a raw-Ethernet loopback test) plus **persistent network/PLCA
configuration** on an Emulated EEPROM (§5.2).

> A more actively developed sibling project,
> [`lan866x-tools`](https://github.com/zabooh/lan866x-tools), builds on this
> firmware and additionally embeds a full **LAN866x SOME/IP (RCP) client** —
> on-board `discovery`/`diag`/`clickdemo`/GPIO/I2C/SPI/DNCP commands that mirror
> the PC-side `lan866x-tools` toolset, plus software NTP time sync. This repo
> stays a lean bridge without the endpoint-tooling/NTP dependencies.

---

## Contents

- [1. What this firmware is for](#1-what-this-firmware-is-for)
- [2. Hardware setup](#2-hardware-setup)
  - [Bridge board: bill of materials](#bridge-board-bill-of-materials)
  - [How `eth0` (LAN865x) is wired](#how-eth0-lan865x-is-wired-from-the-firmware-config)
  - [Network and addressing (default)](#network-and-addressing-default)
  - [Console and cabling](#console-and-cabling)
- [3. Firmware architecture](#3-firmware-architecture)
  - [Block view](#block-view)
  - [The bridge data path](#the-bridge-data-path)
  - [Application state machine (`app.c`)](#application-state-machine-appc)
  - [Persistent config (`env.c`, Emulated EEPROM)](#persistent-config-envc-emulated-eeprom)
  - [Port mirror and SPAN (Wireshark)](#port-mirror-and-span-wireshark)
  - [CLI commands](#cli-commands)
- [4. Building it yourself](#4-building-it-yourself)
- [5. Changing IP and PLCA configuration](#5-changing-ip-and-plca-configuration)
  - [5.1 Persistent: edit the build config and rebuild](#51-persistent-edit-the-build-config-and-rebuild)
  - [5.2 Persistent via the `env` CLI group (recommended)](#52-persistent-via-the-env-cli-group-recommended)
  - [5.3 Volatile runtime via Harmony stack commands](#53-volatile-runtime-via-harmony-stack-commands--plca_node)
- [6. Port mirror: capturing the T1S bus in Wireshark](#6-port-mirror-capturing-the-t1s-bus-in-wireshark)
  - [6.1 Why a mirror is needed](#61-why-a-mirror-is-needed)
  - [6.2 What gets mirrored (both directions, MAC-filtered)](#62-what-gets-mirrored-both-directions-mac-filtered)
  - [6.3 Using it](#63-using-it)
  - [6.4 Limitations](#64-limitations)

---

## 1. What this firmware is for

The board sits between two worlds:

```
   PC / lab network                Bridge (this firmware)            T1S bus
   100BASE-T (RJ45)          ATSAME54P20A + LAN865x + LAN8740     10BASE-T1S (2-wire)
   ┌──────────────┐  100M    ┌───────────────────────────┐  T1S   ┌──────────────┐
   │  Wireshark   │◄────────►│ eth1 (GMAC)   eth0 (LAN865x)│◄──────►│  LAN866x     │
   │  ping        │ .210/.200│   └── MAC bridge (L2) ──┘   │ PLCA   │  endpoint    │
   └──────────────┘          └───────────────────────────┘ node 7  │  192.168.0.54│
                                                                    └──────────────┘
```

It does two jobs:

**a) Transparent L2 bridge.** The two interfaces — `eth0` (the T1S MAC-PHY) and
`eth1` (100BASE-T) — are joined by the Harmony **MAC bridge**, so any PC-side
traffic (ARP, ICMP/ping, mDNS, ...) flows through to the endpoint on the T1S bus
and back, with MAC learning (FDB). From the PC you can simply
`ping 192.168.0.54` and reach the endpoint *through* the bridge as if it were on
the local Ethernet. The bridge does **not** forward manually in application
code — the Harmony MAC bridge handles all L2 forwarding in both directions.

**b) T1S bus analyzer / SPAN port.** The firmware can mirror T1S traffic onto
`eth1` so you can capture the two-wire bus in **Wireshark** on the PC —
including the endpoint's replies *and* the bridge's own requests (`mirror`
command). It also has raw frame dump/logging (`ipdump`, `logstat`), a
raw-Ethernet loopback test (`noip_send`), LAN865x register peek/poke
(`lan_read`/`lan_write`), PLCA node-ID control, and per-interface counters.

---

## 2. Hardware setup

The bridge node is built from a Microchip SAM E54 Curiosity board with one
MikroElektronika Click add-on. The **LAN866x endpoint** on the T1S side is a
*separate* device (the thing you reach through the bridge); it is not part of
the bridge board.

### Bridge board: bill of materials

| Function | Board | Microchip order number |
|---|---|---|
| **MCU host** (Cortex-M4F, runs this firmware) + **onboard 100BASE-T PHY** for `eth1` (GMAC ↔ RJ45) | SAM E54 Curiosity (Ultra) board (ATSAME54P20A, onboard LAN8740A Ethernet) | **DM320210** — *verify against your board* |
| **10BASE-T1S MAC-PHY** for `eth0` (SPI ↔ two-wire bus) | MikroElektronika **Two-Wire ETH Click** (LAN8651) | `MIKROE-xxxx` — *verify on mikroe.com* |

> **`eth1` (100BASE-T) is the host board's onboard Ethernet** — a **LAN8740A**
> PHY on RMII (PHY address 0), driven by the GMAC. No separate PHY daughter
> board is used; the RJ45 on the board edge is the 100BASE-T port. This matches
> the firmware config (`DRV_LAN8740_PHY_*` in `configuration.h`).
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
| endpoint | LAN866x | 192.168.0.54 | /24 | follower |

Both bridge interfaces share one `192.168.0.0/24` subnet (gateway
`192.168.0.1`) — the MAC bridge makes that a single L2 segment. Put the PC's
RJ45 adapter on the same subnet, on an address **other than** `.200`/`.210`/`.54`
(e.g. `192.168.0.220`).

> **PLCA coordinator.** If the T1S side is meant to run with this board as
> coordinator, set the node id to **0** (`plca_node 0` at runtime, or
> `DRV_LAN865X_PLCA_NODE_ID_IDX0` in `configuration.h` for a persistent
> change) — see [§5](#5-changing-ip-and-plca-configuration).

### Console and cabling

1. **Debugger + console:** one USB cable from the PC to the SAM E54 Curiosity
   board's **embedded-debugger** USB port. This is both the programmer
   (PKOB/EDBG) and the virtual COM port for the CLI (**115200 8N1**).
2. **100BASE-T:** the board's **onboard RJ45** (LAN8740A PHY) ↔ the PC's
   Ethernet adapter (the one set to `192.168.0.220`).
3. **T1S:** the two-wire bus from the LAN865x Click to the LAN866x endpoint.

---

## 3. Firmware architecture

Built on **MPLAB Harmony 3** for the ATSAME54P20A. Single-threaded cooperative
superloop (`SYS_Tasks()` in `main.c`); no RTOS, no threads, no locks.

### Block view

```
                       ┌──────────────────────────────────────────────┐
   serial CLI ───────► │ SYS_CMD console: "Test" + "env" groups       │
   (EDBG COM)          │   (app.c / env.c)                            │
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

`APP_Initialize` registers the Telnet auth + a 1 s timer and calls
`Command_Init()` to register the `Test` command group. `APP_Tasks` walks
`INIT → WAIT → SERVICE_TASKS → IDLE`: in `SERVICE_TASKS` it registers the two
packet handlers and calls `env_apply()` (push the persisted network config
into the now-running TCP/IP stack); in `IDLE` it (1) services the async
LAN865x register read/write state machine (`lan_read`/`lan_write`/
`plca_node`), and (2) drains the deferred packet-log ring buffer to the
console (≤10 entries/iteration, so logging never stalls the loop). Captured
frame bytes go to a separate circular pool; the ring uses a lock-free
single-producer/consumer pattern (handlers write, `APP_Tasks` reads).

### Persistent config (`env.c`, Emulated EEPROM)

*(Full walkthrough in [§5.2](#52-persistent-via-the-env-cli-group-recommended).)*

A versioned, CRC-protected record (per-interface IP/mask/gateway/DNS, both
MACs, PLCA node id/count) lives in the **Emulated EEPROM** Harmony library
(added via MCC; reserves the last 16 KB of flash). `ENV_Init()` runs at the
very start of `SYS_Initialize()` in `initialization.c` — **before**
`TCPIP_STACK_Init()` — so a persisted or freshly-seeded MAC is already in
effect when the stack binds its interfaces. On a blank/corrupt EEPROM (e.g.
first flash) it seeds itself from the `configuration.h` compiled defaults,
including a per-board MAC derived from the SAME54's serial number.

### Port mirror and SPAN (Wireshark)

*(Full walkthrough and limitations in [§6](#6-port-mirror-capturing-the-t1s-bus-in-wireshark).)*

`mirror 1` turns on two clone paths so a PC capture on `eth1` sees the full T1S
picture, each filtered by the bridge's own `eth0` MAC to stay duplicate-free:
- **RX mirror** (`mirror_eth0_rx_to_eth1`, app.c): frames addressed to the
  bridge (dst MAC == `eth0`) — the endpoint's replies — are cloned to `eth1`.
- **TX mirror** (`mirror_eth0_tx_hook`, called from the LAN865x egress
  `DRV_LAN865X_PacketTx`): frames the bridge itself originates (src MAC ==
  `eth0`) — its `ping`/ARP — are cloned to `eth1`, protocol-independent.
  Because a node never receives its own TX, hooking the egress is the only way
  to see them.

### CLI commands

Two command groups; type the command name directly (no group prefix needed).

**`Test` group:**

| Command | Description |
|---|---|
| `help` | show this list |
| `timestamp` | firmware build timestamp |
| `mirror [0\|1]` | SPAN: copy T1S (eth0) traffic — RX **and** the bridge's own TX — to eth1 for Wireshark |
| `ipdump [0..3]` | dump RX frames (0=off, 1=eth0, 2=eth1, 3=both) |
| `stats` | per-interface TX/RX software counters |
| `meminfo` | free memory: C-runtime heap (total + largest free block) **and** TCP/IP heap (free/maxblock/highwater, like `heapinfo`) |
| `plca_node [id]` | get/set PLCA node id (0 = coordinator); no arg = show current — **volatile**, see [§5.3](#53-volatile-runtime-via-harmony-stack-commands--plca_node) |
| `lan_read <addr>` / `lan_write <addr> <val>` | LAN865x register access (hex) |
| `noip_send <n> [gap_ms]` / `noip_stat` | raw-Ethernet (EtherType 0x88B5) loopback test + counters |
| `dump <addr> <count>` | memory dump (hex) |
| `logstat` / `logclear` | deferred packet-log statistics / clear |

**`env` group** — persistent config on the Emulated EEPROM (see [§5.2](#52-persistent-via-the-env-cli-group-recommended)):

| Command | Description |
|---|---|
| `showenv` | show the current config: per-interface IP/mask/gw/dns, MAC, PLCA id/count |
| `setenv <key> <val>` | edit the RAM shadow — keys: `ip0/mask0/gw0/dns0`, `ip1/…`, `mac0`/`mac1`, `plca_id`/`plca_cnt` |
| `saveenv` | persist to EEPROM **and** apply (IP/PLCA live; MAC at next reset) |
| `readenv` | reload from EEPROM and apply (discard unsaved edits) |
| `resetenv` | restore the compiled defaults, persist and apply |

Harmony stack commands (`netinfo`, `bridge`, `ping`, `setip`, `setgw`, etc.) are
also available.

---

## 4. Building it yourself

This is a plain **MPLAB X** project (no CMake/Ninja) with a thin shell wrapper
around MPLAB X's own build, plus a **pyOCD**-based flash tool (no MDB/MPLAB X
needed just to program the board).

### 4.1 Tool prerequisites (per machine)

| Requirement | Notes |
|---|---|
| **MPLAB X IDE** | needed once to generate the project's build files (see 4.2) and for the SAME54_DFP device pack |
| **MPLAB XC32** | this firmware was built with XC32 v4.60, under `C:\Program Files\Microchip\xc32\` |
| **Python 3.9+** | `pyserial` for `cli.py`/`smoketest.py`, `pyocd` for `flash.bat` (installed by setup) |
| **Terminal** | the board's EDBG virtual COM port, 115200 8N1 |

### 4.2 One-time setup after cloning

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

### 4.3 Build and flash

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

### 4.4 Smoke test

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

1. `setup.bat` → open+build once in MPLAB X (4.2) → `build.bat` → `flash.bat`.
2. Open the EDBG COM port at 115200 8N1; you should see the build banner.
3. `stats` — confirm `eth0`/`eth1` exist and counters move.
4. `plca_node` — reports the configured node id.
5. From the PC (on `192.168.0.220`): `ping 192.168.0.54` → 0% loss (bridge
   works). Or run `python smoketest.py`.

---

## 5. Changing IP and PLCA configuration

The IP addresses (`eth0` = 192.168.0.200, `eth1` = 192.168.0.210) and the PLCA
parameters can be changed two ways:

- **Persistent, no rebuild — the `env` command group (§5.2).** Backed by the
  Emulated EEPROM (see [§3](#3-firmware-architecture)); survives reset/power-cycle.
  This is the recommended way to change a board's config.
- **Persistent, requires rebuild — edit `configuration.h` (§5.1).** Only
  matters for the *compiled-in* defaults that `env` seeds a blank/freshly
  flashed EEPROM from (`resetenv` also restores these).

### 5.1 Persistent: edit the build config and rebuild

All defaults live in **`firmware/src/config/default/configuration.h`** (an
MCC-generated file). Edit the macros, then rebuild + reflash in MPLAB X.

| Setting | Macro (`configuration.h`) | Default |
|---|---|---|
| eth0 (T1S) IP | `TCPIP_NETWORK_DEFAULT_IP_ADDRESS_IDX0` | `"192.168.0.200"` |
| eth0 subnet mask | `TCPIP_NETWORK_DEFAULT_IP_MASK_IDX0` | `"255.255.255.0"` |
| eth0 gateway | `TCPIP_NETWORK_DEFAULT_GATEWAY_IDX0` | `"192.168.0.1"` |
| eth0 MAC | `TCPIP_NETWORK_DEFAULT_MAC_ADDR_IDX0` | `"00:04:25:01:02:03"` (fallback only — `env` derives the real per-board MAC from the SAME54 serial number, see §3) |
| eth1 (100BASE-T) IP | `TCPIP_NETWORK_DEFAULT_IP_ADDRESS_IDX1` | `"192.168.0.210"` |
| eth1 subnet mask | `TCPIP_NETWORK_DEFAULT_IP_MASK_IDX1` | `"255.255.255.0"` |
| eth1 gateway | `TCPIP_NETWORK_DEFAULT_GATEWAY_IDX1` | `"192.168.0.1"` |
| eth1 MAC | `TCPIP_NETWORK_DEFAULT_MAC_ADDR_IDX1` | `"00:04:25:01:02:04"` (fallback only, see above) |
| PLCA node id | `DRV_LAN865X_PLCA_NODE_ID_IDX0` | `7` |
| PLCA node count | `DRV_LAN865X_PLCA_NODE_COUNT_IDX0` | `8` |

The PLCA node-id macro is the single source of truth: `initialization.c` seeds
the LAN865x driver from it and `app.c`'s `plca_node` command uses it as the
runtime default, so changing the macro is enough.

> **Keep both interfaces on the same subnet as the endpoint and the PC**, since
> the MAC bridge makes them one L2 segment. If this board should be the PLCA
> coordinator, set `DRV_LAN865X_PLCA_NODE_ID_IDX0` to `0`.
>
> **MCC note:** `configuration.h` is generated by MCC. A plain text edit +
> rebuild is fully supported. Only if you *re-run MCC code generation* will it
> be overwritten — in that case make the change in the MCC project (TCP/IP
> network config / LAN865x PLCA) instead.

### 5.2 Persistent via the `env` CLI group (recommended)

The `env` group (`env.c`) keeps a versioned, CRC-protected copy of the
per-interface IP/mask/gateway/DNS, both MACs, and the PLCA node id/count in
the **Emulated EEPROM** — the last 16 KB of flash, reserved by the Emulated
EEPROM library added via MCC. `ENV_Init()` runs at the very start of
`SYS_Initialize()` (before `TCPIP_STACK_Init()`), so a persisted MAC is
already in effect when the stack binds its interfaces.

On first boot (blank/corrupt EEPROM, e.g. right after flashing this firmware
for the first time) `ENV_Init()` seeds the record from the `configuration.h`
defaults (§5.1) — including a **per-board MAC** derived from the SAME54's
128-bit serial number (see [§3](#persistent-config-envc-emulated-eeprom)), so
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

A **PLCA node id/count** change via `setenv plca_id`/`plca_cnt` + `saveenv`
applies immediately (queued through the same LAN865x register write the
`Test` group's `plca_node` command uses) *and* persists. The `Test` group's
own bare `plca_node <id>` (§5.3) is a separate, quicker path for trying a
node id **without** touching the EEPROM at all.

### 5.3 Volatile runtime via Harmony stack commands / `plca_node`

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
> board boots back to whatever `env` has persisted (§5.2), which itself
> defaults to the `configuration.h` values (§5.1) on a blank EEPROM. Use these
> only to try a value before persisting it with `setenv`/`saveenv`.

---

## 6. Port mirror: capturing the T1S bus in Wireshark

The `mirror` command turns the bridge into a SPAN/monitor port: it copies T1S
(`eth0`) traffic onto `eth1` so a PC running Wireshark on its Fast-Ethernet
adapter can see the two-wire bus.

### 6.1 Why a mirror is needed

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

### 6.2 What gets mirrored (both directions, MAC-filtered)

Both directions are cloned to `eth1`, but each is **filtered by the bridge's
own `eth0` MAC** so the capture is **duplicate-free** — frames the MAC bridge
merely *forwards* between the PC and the bus (which the PC already sees
natively on `eth1`) are not mirrored.

| Path | Hook (`app.c`) | Filter | What it captures |
|---|---|---|---|
| **RX mirror** | `mirror_eth0_rx_to_eth1()`, from `pktEth0Handler` | dst MAC **==** `eth0` MAC | frames addressed to the bridge itself — the endpoint's unicast replies to the firmware |
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

### 6.3 Using it

1. On the PC, start **Wireshark** on the Fast-Ethernet adapter connected to
   the bridge's `eth1` (the board's onboard RJ45).
2. On the board CLI: `mirror 1` (turn it on). `mirror` with no argument shows
   the current state; `mirror 0` turns it off.
3. Run anything that talks to the endpoint from the bridge itself (e.g. the
   Harmony `ping 192.168.0.54`) and watch the T1S traffic (both directions)
   appear in Wireshark.

```text
mirror 1                # eth0(T1S) -> eth1 mirror: ON   (RX + the bridge's own TX)
ping 192.168.0.54        # now visible on eth1 in Wireshark: request + reply
mirror 0                # turn it off when done
```

### 6.4 Limitations

- **Exact L2 frames:** both directions clone the real Ethernet frame verbatim
  (header + payload), so MACs and checksums are exactly what went on the wire.
- **The filter relies on transparent bridging:** it assumes the MAC bridge
  does not rewrite source/destination MACs (it doesn't). The reference is the
  `eth0` interface MAC; frames carrying it are treated as the bridge's own.
- **Single-segment copy:** the mirror clones the packet's first data segment.
  The bridge/stack frames involved are single-segment; a hypothetical
  multi-segment frame would be truncated.
- **Broadcast/multicast received on `eth0` is not mirrored** — the bridge
  already forwards it to `eth1`, where the PC sees it natively (see §6.2).
- Mirroring adds one cloned `eth1` transmit per matching frame. It is meant
  for diagnostics — leave it **off** for normal bridging to avoid the extra
  load.
- Mirror state is a runtime toggle (like the §5.2 CLI settings) and defaults
  to **off** on every boot.
