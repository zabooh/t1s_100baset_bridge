# CLI command reference

Every command the bridge firmware registers on its serial console, what it does, and an example.

**How this list was produced:** not from memory, but by reading the six command tables the firmware
registers with Harmony's `SYS_CMD` service. If you add a command, add a row here — and if the two ever
disagree, the source wins:

| Table | Group | Where |
|---|---|---|
| `msd_cmd_tbl` | `Test` | [app.c:640](../firmware/src/app.c#L640) |
| `env_cmd_tbl` | `env` | [env.c:304](../firmware/src/env.c#L304) |
| `lan_cmd_tbl` | `lan` | [lan865x_diag.c:555](../firmware/src/lan865x_diag.c#L555) |
| `noip_cmd_tbl` | `noip` | [noip_test.c:148](../firmware/src/noip_test.c#L148) |
| `mirror_cmd_tbl` | `mirror` | [port_mirror.c:142](../firmware/src/port_mirror.c#L142) |
| `tcpipCmdTbl`, `iperfCmdTbl` | `tcpip`, `iperf` | Harmony TCP/IP stack (generated code) |

That is **22 project commands** plus the stack's own two groups. Output shown below is taken from the
firmware's own format strings, so the shape is exact even where the numbers are illustrative.

**This file is checked mechanically, not by eye:**

```bat
python scripts/cli_doc_check.py          :: PASS/FAIL, exit code 1 on any drift
python scripts/cli_doc_check.py --list   :: just what the firmware registers
```

It catches the three ways a reference like this rots — a command with no section, a section with no
command, and a quick-reference link pointing nowhere. Nobody notices a missing command by reading,
because you only look up commands you already know exist.

---

## 1. Reaching the console

The console runs on the board's **EDBG virtual COM port at 115200 8N1** — over UART, not over the
network. That matters: no register poke, test mode or link failure can lock you out.

```bat
python scripts/cli.py --port COM8 --read 1 "stats"
```

`cli.py` sends one command and collects the reply. Two things to know:

- **The port is exclusive.** A terminal (TeraTerm, PuTTY, the VS Code serial monitor) holding the port
  makes every script fail with `PermissionError(13, 'Access is denied.')`. Close it first.
- **`cli.py` reads until the console goes quiet.** If something on the board prints continuously — say
  `ipdump` is on — use `serial_capture.py`, which reads for a fixed time instead.

Find the port for a given board by its probe serial number rather than by guessing:

```bat
python -c "import serial.tools.list_ports as p;[print(x.device,x.serial_number) for x in p.comports()]"
```

## 2. How the groups work

Harmony registers commands in named groups, but on this build **no group prefix is needed** — type the
command directly. The group only decides which help listing it appears in:

| Command | Lists |
|---|---|
| `help` | the `Test` group, plus a pointer to `lanhelp` |
| `lanhelp` | the LAN865x diagnostics group |

---

## 3. Quick reference

| Command | Group | One line |
|---|---|---|
| [`help`](#help) | Test | list the Test group commands |
| [`timestamp`](#timestamp) | Test | show the firmware build stamp |
| [`uptime`](#uptime) | Test | time since boot/last reset |
| [`history`](#history) | Test | show previously entered CLI commands |
| [`stats`](#stats) | Test | TX/RX counters for `eth0` and `eth1` |
| [`meminfo`](#meminfo) | Test | free memory on the C heap and the TCP/IP heap |
| [`dump`](#dump) | Test | hex-dump MCU memory |
| [`ipdump`](#ipdump) | Test | log received IP packets per interface |
| [`logstat`](#logstat) | Test | deferred packet log statistics |
| [`logclear`](#logclear) | Test | empty the deferred packet log |
| [`showenv`](#showenv) | env | show the stored network/PLCA config |
| [`setenv`](#setenv) | env | change one config key (RAM only) |
| [`saveenv`](#saveenv) | env | persist to EEPROM and apply |
| [`readenv`](#readenv) | env | reload from EEPROM, discarding edits |
| [`resetenv`](#resetenv) | env | back to compiled defaults, persisted |
| [`lanhelp`](#lanhelp) | lan | list the LAN865x commands |
| [`lan_read`](#lan_read) | lan | read a LAN8651 register |
| [`lan_write`](#lan_write) | lan | write a LAN8651 register |
| [`lan_rmw`](#lan_rmw) | lan | read-modify-write with verification |
| [`testmode`](#testmode) | lan | IEEE transmitter test modes 0–4 |
| [`plca_node`](#plca_node) | lan | get/set the PLCA node ID (volatile) |
| [`noip_send`](#noip_send) | noip | send raw Ethernet frames, bypassing IP |
| [`noip_stat`](#noip_stat) | noip | raw-frame TX/RX counters |
| [`mirror`](#mirror) | mirror | mirror the T1S port to `eth1` for Wireshark |
| [`sniffer`](#sniffer) | mirror | mirror ALL `eth0` RX to `eth1`, incl. traffic between other nodes |
| [`bigframe`](#bigframe) | mirror | send one raw oversized frame straight out `eth1` |
| [stack commands](#9-harmony-tcpip-stack-commands) | tcpip, iperf | `netinfo`, `arp`, `ping`, `bridge`, `iperf` … |

---

## 4. Test group

### `help`

```
help
```

Lists the Test group and points at `lanhelp` for the register/test-mode commands. It does **not** list
the `env`, `noip`, `mirror` or stack groups — this reference is the only complete list.

```
Test group commands:
  help                         - Show this help
  timestamp                    - Show build timestamp
  ipdump <mode>                - Dump RX IP packets (0=off, 1=eth0, 2=eth1, 3=both)
  stats                        - Show TX/RX software counters for eth0 and eth1
  dump <addr> <count>          - Dump memory (hex addr, count)
  logclear                     - Clear deferred packet log buffer
  logstat                      - Show deferred log statistics

LAN865x registers, test modes, PLCA: see 'lanhelp'
```

### `timestamp`

```
timestamp
```

Prints the `__DATE__`/`__TIME__` of the build. **Use it before every measurement** — it is the only way
to tell from the outside which firmware is actually running.

```
======================================
T1S Packet Sniffer - Build Info
Build Timestamp: Aug 18 2026 17:58:12
======================================
```

One caveat worth knowing: those macros are expanded in `app.c`, so an **incremental build that did not
recompile `app.c` leaves the stamp at the older date** while the linked image is new. `build.bat`
touches `app.c` before building for exactly this reason.

### `uptime`

```
uptime
```

Time since boot/last reset, human-readable. The fast way to tell "the board is still the same process
that was running before" from "it silently rebooted (watchdog, assert loop, `pyocd reset`) and only
looks the same" — nothing else in the normal log surfaces a silent restart.

```
uptime: 0d 00:14:22  (862 s since boot/last reset)
```

### `history`

```
history
```

Shows the last `CMD_HISTORY_DEPTH` (20) CLI commands entered, oldest first, including this `history`
call itself.

```
command history: 5/20, oldest first (this command included)
   1: stats
   2: mirror 1
   3: mirror 0
   4: testmode
   5: history
```

### `stats`

```
stats
```

Software counters of both interfaces, straight from the stack. The cheapest health check there is — and
unlike `lan_read` it does **not** touch the SPI path, so it is safe to poll during a throughput test.

```
eth0 TX: ok=1482 err=0 qFull=0 pend=0
eth0 RX: ok=1517 err=0 nobufs=0 pend=0
eth1 TX: ok=1519 err=0 qFull=0 pend=0
eth1 RX: ok=1486 err=0 nobufs=0 pend=0
```

`qFull` climbing means the driver queue is the bottleneck; `nobufs` means the RX pool ran dry — check
`meminfo` next.

### `meminfo`

```
meminfo
```

Free memory on both heaps: the C runtime's and the TCP/IP stack's.

```
C-runtime heap: total=98304  largest free block=61440  (nano-malloc; no exact free count)
TCP/IP heap:    size=39264  free=21496  maxblock=20480  highwater=17768
```

**The label `nano-malloc` in that line is wrong.** This build links musl's allocator —
`liblmalloc-musl.a(lite_malloc.o)`, 22 references in the map file — not newlib's nano-malloc. The
consequence is practical: `lite_malloc` never frees back, so "largest free block" only ever shrinks, and
the `highwater` figure of the TCP/IP heap is the number to watch for a leak. `_get_heap_left_space` is
linked and would give an exact figure; the command does not use it yet.

### `dump`

```
dump <address_hex> <count>
```

Hex-dumps `count` bytes of MCU memory, 16 per line with an ASCII column.

```
dump 0x20000000 32
```
```
Memory dump: 0x20000000  32 bytes
20000000: 00 00 20 20 c1 03 00 00 00 00 00 00 ff ff ff ff     ..  ............
20000010: 02 00 00 00 6c 61 6e 38 36 35 31 00 00 00 00 00     ....lan8651.....
```

No bounds check — a bad address faults the MCU. Useful ranges: `0x20000000` (SRAM), `0x00000000`
(flash).

### `ipdump`

```
ipdump <0|1|2|3>       0=off, 1=eth0, 2=eth1, 3=both
```

Logs one line per received IP packet into a ring buffer that a background task drains, so the packet
path is not slowed by console output.

```
ipdump 3
```
```
IP Layer Dump activated on eth0 and eth1
E0:12 len=64 ts=1755534 ms
E1:13 len=98 ts=1755536 ms
```

`E0`/`E1` is the interface, the number is the sequence, `ts` the millisecond timestamp. **Turn it off
with `ipdump 0` before any timing measurement** — and remember `cli.py` will not return while this is
running.

### `logstat`

```
logstat
```

Whether the deferred log kept up. `overflows` greater than zero means lines were dropped, so the
`ipdump` output above is incomplete.

```
[LOG] total=204 pending=0 overflows=0 bufsize=64
[LOG] pool_offset=0 pool_size=8192 (64 frames x 128 bytes)
```

### `logclear`

```
logclear
```

Empties the ring buffer and zeroes the counters — run it before a measurement so `logstat` afterwards
describes only that run.

```
[LOG] ring buffer cleared
```

---

## 5. Persistent configuration (`env`)

IP addresses, MAC addresses, the PLCA parameters and the mirror start state live in the emulated
EEPROM. The pattern is always the same: `setenv` edits a **RAM shadow**, `saveenv` persists and applies
it. An edit you do not save is gone at the next reset.

The record is versioned and CRC-protected, and the loader demands an **exact** version match — an older
record would be discarded and the compiled defaults seeded instead. That is not harmless here: the
compiled default PLCA node id is **7**, while a bridge acting as coordinator runs **0**, a value that can
only come from the EEPROM. A firmware update that reset it would silently stop the bridge being the
coordinator, with nothing in the log pointing at the EEPROM. Records are therefore **migrated**, not
discarded: v3 → v4 carries every field over and reports

```
env: migrated v3 record to v4 (settings kept, mirror=0)
```

### `showenv`

```
showenv
```

```
env (RAM shadow):
  env   id EBRG  version 4  crc ok  |  firmware id EBRG  version 4  t1s_100baset_bridge
  eth0  ip 192.168.0.200  mask 255.255.255.0  gw 192.168.0.1  dns 192.168.0.1
  eth1  ip 192.168.0.210  mask 255.255.255.0  gw 192.168.0.1  dns 192.168.0.1
  eth0  mac 00:04:25:1C:A0:02
  eth1  mac 00:04:25:1C:A0:01  (applied at boot)
  plca  id 0  count 8  (eth0/T1S)
  mirror OFF at boot  (now: ON)
  (saveenv = persist+apply, readenv = reload, resetenv = defaults)
```

The first line is the **environment identity**, and it names two things that can disagree.
`id`/`version` is the record that was **found in the EEPROM at boot**; `firmware id`/`version` is
what this firmware writes. They differ exactly when the stored record was rejected — a different
firmware variant wrote it, or the layout changed — and in that case the compiled defaults are in
use. That matters here: the default PLCA node id is 7, while the bridge has to run as coordinator
(id 0), so a rejected record silently costs the beacons. `crc` says whether the stored record was
intact.

The id is four characters and identifies the **firmware variant**, not just "this is an env
record": variants share the EEPROM offset but not the layout. `EBRG` is this bridge; `t1s_ptp_bridge`
stores `ptp_auto`/`ptp_ival` where this one stores `mirror`. `LANE` is the id written before ids
were per-variant — still read when version and CRC match this layout, and rewritten as `EBRG` on
the next `saveenv`.

The mirror line names **two** states on purpose, because they can differ: what the board will do after
a reset, and what it is doing right now. `mirror 1` changes only the second.

### `setenv`

```
setenv <key> <value>
```

| Key group | Keys | Format | Takes effect |
|---|---|---|---|
| IP | `ip0` `mask0` `gw0` `dns0`, `ip1` `mask1` `gw1` `dns1` | dotted quad | on `saveenv` |
| MAC | `mac0`, `mac1` | `XX:XX:XX:XX:XX:XX` | **after a reset** — the stack binds the MAC at init |
| PLCA | `plca_id` (0–254), `plca_cnt` (1–255) | decimal | on `saveenv` |
| Mirror | `mirror` | `0` or `1` | **at the next boot** — `MIRROR_Initialize()` reads it |

`0` = `eth0` = the T1S side (LAN8651), `1` = `eth1` = the 100BASE-T side (GMAC/LAN8740A).

```
setenv ip1 192.168.0.210
```
```
setenv: ip1 = 192.168.0.210 (RAM only; 'saveenv' to persist)
```

### `saveenv`

```
saveenv
```

Writes the shadow to EEPROM **and** applies it live. Rejected values are reported by `setenv`, not here.

```
saveenv: persisted to EEPROM and applied
```

### `readenv`

```
readenv
```

Reloads from EEPROM and applies — the way back if you edited yourself into a corner. **Discards unsaved
edits**, which is the point.

```
readenv: reloaded from EEPROM and applied.
```

### `resetenv`

```
resetenv
```

Compiled defaults, persisted and applied in one step.

```
resetenv: restored compiled defaults, persisted and applied.
```

---

## 6. LAN865x diagnostics (`lan`)

Generic register access to the LAN8651, the IEEE transmitter test modes, and the PLCA node ID. The
module ([lan865x_diag.c](../firmware/src/lan865x_diag.c)) is deliberately self-contained: two files, one
`LAN865X_DIAG_Initialize()` and one `LAN865X_DIAG_Tasks()` call, and it drops into any other LAN865x
project.

### Address encoding

**Upper 16 bits = MMS (memory map selector), lower 16 bits = register offset.**

| MMS | Bank | Example |
|---|---|---|
| 0 | Open Alliance standard (`OA_CONFIG0`, `OA_STATUS0/1`) | `0x00000000` |
| 1 | MAC (wall clock, TX timestamps) | `0x00010077` |
| 2 | PHY PCS | `0x00020000` |
| **3** | **PHY PMA/PMD — the test-mode registers** | `0x000308FB` |
| 4 | PHY vendor specific (PLCA, SQI, CFD) | `0x0004CA02` |
| 10 | Misc (event capture, 1PPS, `PADCTRL`, device ID) | `0x000A0094` |

### `lanhelp`

```
lanhelp
```

```
LAN865x diagnostics commands:
  lan_read  <addr>             - Read  LAN865X register (hex address)
  lan_write <addr> <value>     - Write LAN865X register (hex addr, hex value)
  lan_rmw <addr> <mask> <val>  - Read-modify-write + verify: new=(old&~mask)|val
  testmode [0..4] [seconds]    - IEEE TX test mode, verified by readback (no arg = show)
  plca_node [id]               - Get/set PLCA node ID (no arg = show current)

Address = (MMS << 16) | offset. MMS 3 = PHY PMA/PMD, MMS 4 = vendor specific.
```

### `lan_read`

```
lan_read <address_hex>
```

```
lan_read 0x0004CA02
```
```
LAN865X Read OK: Addr=0x0004CA02 Value=0x00000800
```

That register is `PLCA_CTRL1`: `NODE_CNT << 8 | NODE_ID`, so `0x0800` means 8 nodes, ID 0 — the
coordinator.

### `lan_write`

```
lan_write <address_hex> <value_hex>
```

```
lan_write 0x000308FB 0x2000
```
```
LAN865X Write OK: Addr=0x000308FB Value=0x00002000
```

**`Write OK` means "the transaction completed", not "the register holds that value".** Always read
back. `testmode` and `lan_rmw` do it for you; plain `lan_write` does not.

### `lan_rmw`

```
lan_rmw <address_hex> <mask_hex> <value_hex>       new = (old & ~mask) | value
```

For registers where several control bits share a word — `T1SPMACTL` above all. It reads, modifies,
writes and then reads back, comparing **only the masked bits**.

```
lan_rmw 0x000308F9 0x00000001 0x00000001
```
```
LAN865X RMW OK: Addr=0x000308F9 Mask=0x00000001 Value=0x00000001 Final=0x00000001
[VERIFY] PASS addr=0x000308F9 masked=0x00000001 (mask 0x00000001)
```

Two things the command tells you itself: a mask of `0` changes nothing (use `lan_write`), and bits in
`value` **outside** the mask are passed through to the register — it warns, it does not filter. A
self-clearing bit such as `RST` legitimately reports `[VERIFY] FAIL`.

### `testmode`

```
testmode                  show the current mode
testmode <0..4>           set it, with automatic readback
testmode <0..4> <1..600>  ... and revert on its own after that many seconds
```

The IEEE 802.3-2022 §147.5.2 transmitter test modes, in hardware. Each produces a **continuous, defined
pattern with no user traffic** — what you need for level, jitter, droop and spectrum measurements.

| Mode | What it qualifies | Instrument |
|---|---|---|
| 0 | normal operation | — |
| 1 | output voltage, timing jitter | oscilloscope |
| 2 | output droop | oscilloscope |
| 3 | PSD mask / transmitter distortion | spectrum analyser |
| 4 | transmitter high impedance | bus measurement without this transmitter |

```
testmode 1 30
```
```
[TESTMODE] requesting 1 - test mode 1 (output voltage / timing jitter) (T1STSTCTL=0x00002000)
LAN865X Write OK: Addr=0x000308FB Value=0x00002000
LAN865X Read OK: Addr=0x000308FB Value=0x00002000
[VERIFY] PASS addr=0x000308FB masked=0x00002000 (mask 0x0000E000)
[TESTMODE] the T1S link is down while this mode is active
[TESTMODE] auto-revert armed in 30 s
```

**Modes 1–4 take the bridged link down by design.** The timeout exists because a forgotten test mode is
expensive to find later: the link simply never comes up, and nothing in the normal log points at a test
register. Deeper background, measurement setup and the protocol per mode:
[LAN8651_TEST_MODES.md](LAN8651_TEST_MODES.md).

### `plca_node`

```
plca_node          show
plca_node <id>     set, live and volatile
```

```
plca_node
```
```
[PLCA] current node ID: 0 (NODE_CNT=8)
```

Node ID 0 is the **coordinator** — it generates the beacon, so exactly one node on the bus may have it.
This command is a live override that does **not** survive a reset; for a permanent change use
`setenv plca_id 0` followed by `saveenv`.

### Behaviour and limits of all LAN commands

These four properties bite anyone scripting against the console, so they are worth stating plainly:

1. **They are asynchronous.** The handler only sets a state; the TC6 transaction runs in the app task
   and answers through a callback. Wait for the reply line before sending the next command.
2. **They are served only in `APP_STATE_IDLE`.** In any other state the command sits there and it looks
   as though nothing happened.
3. **One operation at a time.** A second command arriving first is **rejected**, not queued:
   `ERROR: Previous LAN operation still in progress`.
4. **The timeout is 200 ms** (`LAN_TIMEOUT_MS`), and every access — read *and* write — runs
   `protected = true`.

And one operational rule: **do not poll `lan_read` while traffic is running.** Register access shares
the TC6/SPI service path with the data path; on a sibling firmware a `lan_read` every 200 ms alongside
iperf cost about **5 % UDP packet loss**, while `stats` cost nothing measurable. Use `stats` for runtime
observation.

---

## 7. Raw-frame test (`noip`)

EtherType `0x88B5` frames built by hand and handed to the driver, bypassing the whole TCP/IP stack. When
a question is about the wire rather than about IP, this is the cleanest generator — and the best source
of reproducible scope pictures.

### `noip_send`

```
noip_send <count> [gap_ms]
```

```
noip_send 5 100
```
```
[NoIP-TX] start count=5 gap_ms=100
[NoIP-TX] sent seq=1
[NoIP-TX] sent seq=2
[NoIP-TX] sent seq=3
[NoIP-TX] sent seq=4
[NoIP-TX] sent seq=5
```

Received frames of the same EtherType are logged through the packet log:

```
[NoIP-RX] #3 seq=3 from 00:04:25:1C:A0:02 len=64 ts=1755534 ms
```

### `noip_stat`

```
noip_stat
```

```
[NoIP] TX=5  RX=5
```

TX and RX apart tells you which direction lost frames — the counters are the device's own view, which
outranks any host-side capture when the two disagree.

---

## 8. Port mirror (`mirror`, `sniffer`, `bigframe`)

### `mirror`

```
mirror          show the current state
mirror 0|1      off / on
```

Copies traffic of the T1S port (`eth0`) to `eth1`, so a PC on the 100BASE-T side sees the T1S bus in
Wireshark. **Both directions**: frames from the bus *and* the bridge's own transmissions.

```
mirror 1
```
```
eth0(T1S)->eth1 mirror: ON
  Capture on the PC (eth1) in Wireshark to see the T1S bus traffic:
  RX (endpoint -> bridge: replies/ARP) AND the bridge's own TX.
```

**Surviving a reset.** `mirror 1` is deliberately volatile. To have the board come up mirroring:

```
setenv mirror 1
saveenv
```

`MIRROR_Initialize()` reads that value at boot and announces it with
`MIRROR: eth0(T1S)->eth1 mirror enabled from env`. It is the same split as `plca_node` against
`setenv plca_id`: the command changes the live state, the env decides how the board starts. Default is
off, because every mirrored frame costs a packet-pool entry.

**The TX half depends on a hand-written patch in generated code.** `DRV_LAN865X_PacketTx()` in
`drv_lan865x_api.c` declares `mirror_eth0_tx_hook` as `extern` and calls it. An MCC *Generate Code* run
removes that call silently, and the symptom is misleading: the mirror still shows frames coming *from*
the bus but no longer the bridge's own — which reads like a half-working mirror, not a missing patch.
`python scripts/test_mirror.py` checks exactly this and should run after every regeneration.

`mirror` is pointless while a test mode is active — the link is down. Revert first, then measure.

### `sniffer`

```
sniffer          show the current state
sniffer 0|1      off / on
```

Same mechanism as `mirror`, but drops the destination-MAC filter: it copies **every** `eth0` RX frame
to `eth1`, including traffic between two other T1S nodes that never touches this bridge. Only possible
because the LAN865x driver already runs promiscuous. Also disables the T1S transmitter
(`T1SPMACTL.TXD`) for the duration — a passive tap — so the bridge sends nothing of its own and pauses
forwarding while it is on.

```
sniffer 1
```
```
eth0(T1S)->eth1 sniffer: ON
  Capture on the PC (eth1) in Wireshark to see ALL T1S bus traffic,
  including frames between other nodes that do not involve this bridge.
  T1S transmitter disabled (passive tap) - forwarding is paused meanwhile.
```

Frames over 1514 bytes are truncated before mirroring, not dropped — a larger frame passed through
whole was found to stall the PC-side USB Ethernet adapter/Npcap (see `FALLSTRICKE.md`, 2026-08-27).

`sniffer` and `mirror` share the same 8-entry packet pool and are mutually exclusive in practice — both
being about the T1S RX path, running one while debugging the other just wastes pool entries.

### `bigframe`

```
bigframe <total_frame_len_bytes>   (60..9000)
```

Diagnostic only, unrelated to `mirror`/`sniffer` as such: builds and sends **one** raw Ethernet frame of
the given total length straight out `eth1` via `DRV_GMAC_PacketTx()`, bypassing the TCP/IP stack, T1S,
and the mirror/sniffer pool entirely. Destination is broadcast, source is `eth1`'s own MAC, EtherType
`0xFEED` (deliberately unregistered, unambiguous in a capture), payload is an incrementing byte pattern
so length/content are easy to verify in Wireshark.

Sole purpose: isolate whether an oversized frame on `eth1` alone reproduces a PC-side symptom — rules
out T1S/mirror/iperf as the cause when it does.

---

## 9. Harmony TCP/IP stack commands

Two more groups come from the Harmony stack itself. They are Harmony's, not this project's, and **which
of them exist depends on the MCC configuration** — `help` on the device is the authority. Verified from
`configuration.h` for this build:

| Feature | Macro | State | Effect on the console |
|---|---|---|---|
| ICMP client | `TCPIP_STACK_USE_ICMP_CLIENT` | **on** | `ping` works from the device |
| ICMP server | `TCPIP_STACK_USE_ICMP_SERVER` | **on** | the device answers pings |
| MAC bridge | `TCPIP_STACK_USE_MAC_BRIDGE` | **on** | `bridge` shows the forwarding table |
| iperf | `TCPIP_STACK_USE_IPERF` | **on** | the whole `iperf` group |
| DHCP client | `TCPIP_STACK_USE_DHCP_CLIENT` | **off** | no `dhcp` — both interfaces are static |
| DNS | `TCPIP_STACK_USE_DNS` | **off** | no `dnsc`/`dnss` |

The ones used in practice on this board:

| Command | What it does |
|---|---|
| `netinfo` | address, mask, gateway, MAC and link state per interface |
| `if <name> up\|down` | bring an interface up or down |
| `ping <ip>` | ping from the device |
| `arp <name> list` | the ARP cache |
| `macinfo` | MAC-level statistics — the counters below the stack |
| `bridge` | the L2 bridge's forwarding database |
| `heapinfo` | stack heap, same figures as `meminfo`'s second line |
| `stack on\|off` | stop and restart the stack |
| `tcp`, `udp` | open sockets |

The `iperf` group, for throughput:

| Command | Usage |
|---|---|
| `iperf` | start a measurement |
| `iperfk` | `iperfk <-i index>` — stop it |
| `iperfi` | `iperfi -a <address> <-i index>` — pick the interface |
| `iperfs` | `iperfs <-tx size> <-rx size> <-i index>` — buffer sizes |

Worked iperf examples, including the host side, are in [README.md](../README.md).

---

## 10. Pitfalls worth reading once

- **`Write OK` is not `value stored`.** Read back, every time. Two of this project's wrong conclusions
  came from trusting a write confirmation — one register was rejected on every attempt while the
  readback of a *different* register claimed everything was armed.
- **Never probe a register bank with a register that may legitimately read 0.** An earlier analysis read
  `0x00030001` and `0x00030002` — legacy Clause-45 registers that read 0 on a pure 10BASE-T1S PHY — and
  concluded MMS 3 needed indirect addressing. It does not. Use a **write-readback on a writable bit**.
- **PLCA lives on MMS 4, not MMS 2.** `PLCA_CTRL1` is `0x0004CA02`. Documentation from the AN1847
  context names `0x0200004A`; for this project that is demonstrably wrong.
- **A command whose effect depends on what ran before it is broken**, even if it usually works. Check a
  new command in isolation as well as inside a sequence.
- **"No output" and "console busy" look identical from a script.** If `cli.py` hangs, first check
  whether a terminal holds the port, then whether `ipdump` is on.
- **The console cannot lock you out.** It is UART, not network. Whatever you do to the PHY, the way back
  to `testmode 0` stays open.

---

## See also

| Question | File |
|---|---|
| Hardware, architecture, iperf and host setup | [README.md](../README.md) |
| Test modes: what each qualifies, setup, protocol | [LAN8651_TEST_MODES.md](LAN8651_TEST_MODES.md) |
| Build, flash, register access, known pitfalls (German) | [CLAUDE.md](../CLAUDE.md) |
