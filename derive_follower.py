#!/usr/bin/env python3
"""derive_follower.py - build follower/ out of the bridge project, mechanically.

The follower project under follower/ was not written from scratch: it is the
bridge project with one interface removed and the bridge-only parts taken out.
This script is that derivation, executable, so the difference between the two
projects is a readable list instead of tribal knowledge. Run it on an empty tree
to reproduce follower/, or read it to see exactly what the follower is not.

    python derive_follower.py            derive (refuses if follower/ exists)
    python derive_follower.py --force    delete follower/ first, then derive

Stage 1, a faithful copy
    firmware/ -> follower/firmware/, minus build output, with the project, its MCC
    model directory and every reference to the project name renamed to
    T1S_Follower. Then its own build.bat/flash.bat, which reach the shared python
    tools at the repo root through %~dp0.. so there is one copy of those.

Stage 2, one interface
    The bridge has eth0 = LAN865x/T1S and eth1 = GMAC/100BASE-T with a MAC bridge
    between them. The follower has only eth0, so:
      - TCPIP_STACK_NETWORK_INTERAFCE_COUNT 2 -> 1
      - TCPIP_STACK_USE_MAC_BRIDGE commented out (tcpip_mac_bridge.c then compiles
        to nothing; its TCPIP_MAC_BRIDGE_* settings are left in place but inert)
      - the Index 1 entry drops out of TCPIP_HOSTS_CONFIGURATION, along with
        s_macAddrStr1 and the second env_mac_str() call in the hand-patched
        ENV_Init() block
      - the GMAC and MAC_BRIDGE entries drop out of TCPIP_STACK_MODULE_CONFIG_TBL

Stage 3, no bridge role
    port_mirror.c/.h and ptp_gm.c/.h are deleted - a mirror needs a second
    interface, and the grandmaster is the bridge's job (two masters on one segment
    is a fault, not a redundancy). app.c loses the eth1 receive handler, the
    mirror hook call and the grandmaster calls; noip_test.c loses its
    MIRROR_RawTx() clone; env.c drops to one interface and loses the ptp_auto /
    ptp_ival keys, with its own record magic ('LANF') so a bridge record cannot be
    read as a follower one.

    The generated drv_lan865x_api.c also loses the bridge's hand-patched call to
    mirror_eth0_tx_hook() - without that the follower does not even link, which is
    a good illustration of what CLAUDE.md section 6 warns about.

What deliberately stays: the LAN865x diagnostics (lan_read/lan_write/lan_rmw/
testmode/plca_node), the raw-frame test, the deferred packet log, the memory and
heap commands, and the persistent configuration in the emulated EEPROM.

Every replacement is anchored on exact text and the script stops at the first
anchor it cannot find, so a half-derived tree is not a possible outcome.

Notes for whoever runs this again:
  - The exclusion list must be anchored at the .X directory. Matching "debug"
    anywhere also eats src/config/default/system/debug/, and the build then fails
    on a missing sys_debug.h, which reads like a broken copy rather than a bad
    filter.
  - Windows hands out transient locks on a freshly copied tree, so the first
    rename after copytree can fail with WinError 5. It is retried.
  - MCC-generated files are CRLF while the hand-written ones here are LF, so
    every file is normalised on read and written back in its original convention.
    Without that, anchors with newlines match nothing.
"""
import argparse
import io
import os
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC_FW = ROOT / "firmware"
DST = ROOT / "follower"
DST_FW = DST / "firmware"
SRC = DST_FW / "src"
CFG = SRC / "config" / "default"

OLD_NAME = "T1S_100BaseT_Bridge"
NEW_NAME = "T1S_Follower"
SKIP_DIRS = {"dist", "build", "debug", "__pycache__"}
TEXT_SUFFIXES = {".xml", ".mk", ".properties", ".yml", ".bash", ".bak0", ".mc3", ".mc4", ".bat", ".txt"}
OBJDIR = "${OBJECTDIR}/_ext/1360937237/"

CRLF = chr(13) + chr(10)
LF = chr(10)
changes = []


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def read_norm(path):
    """Read a file normalised to LF, plus the convention to restore on write.

    Only a file that is uniformly CRLF gets converted back, because some
    MCC bookkeeping files are mixed (a couple of CRLF lines among LF ones) and
    blanket conversion would rewrite lines nobody asked to touch - which showed
    up as a byte difference when this script was checked against the tree it is
    supposed to reproduce."""
    with io.open(path, "r", encoding="utf-8", newline="") as fh:
        t = fh.read()
    crs, lfs = t.count(chr(13)), t.count(LF)
    uniform_crlf = crs > 0 and crs == lfs
    return t.replace(CRLF, LF), (CRLF if uniform_crlf else LF)


def write_norm(path, text, nl):
    with io.open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text.replace(LF, nl) if nl == CRLF else text)


def edit(path, pairs):
    t, nl = read_norm(path)
    orig = t
    missing = [old[:70] for old, _ in pairs if old not in t]
    if missing:
        print("ANCHOR MISSING in %s:" % path.name, file=sys.stderr)
        for m in missing:
            print("   %r" % m, file=sys.stderr)
        sys.exit(1)
    for old, new in pairs:
        t = t.replace(old, new)
    if t != orig:
        write_norm(path, t, nl)
        changes.append(path.relative_to(ROOT))


def cut_between(path, start_marker, end_marker, expect=1):
    t, nl = read_norm(path)
    count = 0
    while True:
        i = t.find(start_marker)
        if i < 0:
            break
        j = t.find(end_marker, i)
        if j < 0:
            sys.exit("%s: end marker %r not found after start" % (path.name, end_marker))
        t = t[:i] + t[j + len(end_marker):]
        count += 1
    if count != expect:
        sys.exit("%s: cut %d block(s), expected %d" % (path.name, count, expect))
    write_norm(path, t, nl)
    changes.append(path.relative_to(ROOT))


def rename_retry(src, dst, attempts=5, delay=2.0):
    for i in range(attempts):
        try:
            src.rename(dst)
            return
        except PermissionError:
            if i == attempts - 1:
                raise
            print("  rename denied, retrying (%d/%d)" % (i + 1, attempts))
            time.sleep(delay)


# --------------------------------------------------------------------------
# stage 1: copy, rename, own scripts
# --------------------------------------------------------------------------
def ignore(dirpath, names):
    here = Path(dirpath)
    at_project_root = here.name.endswith(".X")
    return [n for n in names
            if (here / n).is_dir() and (n == "__pycache__" or (at_project_root and n in SKIP_DIRS))]


def stage1_copy():
    print("== stage 1: copy and rename ==")
    shutil.copytree(SRC_FW, DST_FW, ignore=ignore)
    old_x, new_x = DST_FW / (OLD_NAME + ".X"), DST_FW / (NEW_NAME + ".X")
    rename_retry(old_x, new_x)
    for p in sorted(new_x.iterdir()):
        if OLD_NAME in p.name:
            rename_retry(p, new_x / p.name.replace(OLD_NAME, NEW_NAME))
    touched = 0
    for dirpath, dirnames, filenames in os.walk(DST_FW):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            p = Path(dirpath) / fn
            if p.suffix.lower() not in TEXT_SUFFIXES:
                continue
            # Raw read/write here: renaming the project needs no newline-bearing
            # anchors, so there is no reason to touch line endings at all.
            try:
                with io.open(p, "r", encoding="utf-8", newline="") as fh:
                    t = fh.read()
            except (UnicodeDecodeError, OSError):
                continue
            if OLD_NAME in t:
                with io.open(p, "w", encoding="utf-8", newline="") as fh:
                    fh.write(t.replace(OLD_NAME, NEW_NAME))
                touched += 1
    print("   project name rewritten in %d files" % touched)

    for name, subs in (
        ("build.bat", [
            ('set "MPLAB_DIR=%SCRIPT_DIR%firmware\\T1S_100BaseT_Bridge.X"',
             'set "MPLAB_DIR=%SCRIPT_DIR%firmware\\T1S_Follower.X"'),
            ('set "PROJ_NAME=T1S_100BaseT_Bridge"', 'set "PROJ_NAME=T1S_Follower"'),
            ('set "COMPILER_CONFIG=%SCRIPT_DIR%setup_compiler.config"',
             'set "COMPILER_CONFIG=%SCRIPT_DIR%..\\setup_compiler.config"'),
            ('python "%SCRIPT_DIR%build_summary.py"', 'python "%SCRIPT_DIR%..\\build_summary.py"'),
            ('copy /Y "%HEX_PATH%" "%SCRIPT_DIR%release\\T1S_100BaseT_Bridge.hex" >nul',
             'copy /Y "%HEX_PATH%" "%SCRIPT_DIR%release\\T1S_Follower.hex" >nul'),
            ('echo Released: %SCRIPT_DIR%release\\T1S_100BaseT_Bridge.hex',
             'echo Released: %SCRIPT_DIR%release\\T1S_Follower.hex'),
            ('::      firmware\\T1S_100BaseT_Bridge.X', '::      firmware\\T1S_Follower.X'),
        ]),
        ("flash.bat", [
            ('set "PROJECT=bridge"', 'set "PROJECT=follower"'),
            ('set "TOOL=%~dp0flash_same54.py"', 'set "TOOL=%~dp0..\\flash_same54.py"'),
            ('set "HEX=%~dp0firmware\\T1S_100BaseT_Bridge.X\\dist\\default\\production\\T1S_100BaseT_Bridge.X.production.hex"',
             'set "HEX=%~dp0firmware\\T1S_Follower.X\\dist\\default\\production\\T1S_Follower.X.production.hex"'),
            ('rem  flash.bat - flashes the T1S<->100BASE-T bridge firmware onto the SAM E54',
             'rem  flash.bat - flashes the T1S PTP follower firmware onto its SAM E54'),
        ]),
    ):
        t, nl = read_norm(ROOT / name)
        missing = [a for a, _ in subs if a not in t]
        if missing:
            sys.exit("%s: pattern(s) not found: %r" % (name, missing[:2]))
        for a, b in subs:
            t = t.replace(a, b)
        write_norm(DST / name, t, nl)
    print("   wrote follower/build.bat and follower/flash.bat")


# --------------------------------------------------------------------------
# stage 2: one network interface
# --------------------------------------------------------------------------
def stage2_single_interface():
    print("== stage 2: single interface ==")
    conf = CFG / "configuration.h"
    t, nl = read_norm(conf)
    old_count = "#define TCPIP_STACK_NETWORK_INTERAFCE_COUNT  \t2"
    if old_count not in t:
        sys.exit("interface count define not found verbatim - check configuration.h")
    t = t.replace(old_count, "#define TCPIP_STACK_NETWORK_INTERAFCE_COUNT  \t1")
    bridge_def = LF + "#define TCPIP_STACK_USE_MAC_BRIDGE"
    if bridge_def not in t:
        sys.exit("MAC bridge define not found")
    t = t.replace(bridge_def, LF +
                  "/* The follower has a single interface, so there is nothing to bridge. The" + LF +
                  " * TCPIP_MAC_BRIDGE_* settings below are left in place but inert without this" + LF +
                  " * define - tcpip_mac_bridge.c compiles to nothing. */" + LF +
                  "/* #define TCPIP_STACK_USE_MAC_BRIDGE */", 1)
    write_norm(conf, t, nl)
    changes.append(conf.relative_to(ROOT))

    init = CFG / "initialization.c"
    cut_between(init, "    /*** Network Configuration Index 1 ***/", "MAC_DRIVER_IDX1,\n    },\n")
    edit(init, [
        ("static char s_macAddrStr1[18] = TCPIP_NETWORK_DEFAULT_MAC_ADDR_IDX1;\n", ""),
        ("    {TCPIP_MODULE_MAC_PIC32C,       &tcpipGMACInitData},            // TCPIP_MODULE_MAC_PIC32C\n", ""),
        ("    {TCPIP_MODULE_MAC_BRIDGE,       &tcpipBridgeInitData},      // TCPIP_MODULE_MAC_BRIDGE \n", ""),
        ("   env_mac_str(0, s_macAddrStr0);\n   env_mac_str(1, s_macAddrStr1);\n",
         "   env_mac_str(0, s_macAddrStr0);\n"),
        ("    * them. The MAC is env-stored and changeable via 'setenv mac0/mac1' + 'saveenv'. */\n",
         "    * them. The MAC is env-stored and changeable via 'setenv mac0' + 'saveenv'. */\n"),
    ])


# --------------------------------------------------------------------------
# stage 3: no bridge role
# --------------------------------------------------------------------------
def stage3_strip_bridge_role():
    print("== stage 3: remove the bridge role ==")
    import re

    for name in ("port_mirror.c", "port_mirror.h", "ptp_gm.c", "ptp_gm.h"):
        p = SRC / name
        if p.exists():
            p.unlink()
            changes.append(Path("deleted") / name)

    mk = DST_FW / (NEW_NAME + ".X") / "nbproject" / "Makefile-default.mk"
    mk_text, mk_nl = read_norm(mk)
    lines, out, dropped = mk_text.splitlines(keepends=True), [], 0
    i = 0
    while i < len(lines):
        ln = lines[i]
        if any(ln.startswith(OBJDIR + m + ".o: ../src/%s.c" % m) for m in ("port_mirror", "ptp_gm")):
            i += 6                       # target + 4 body lines + blank separator
            dropped += 1
            continue
        if i < 100:
            for m in ("port_mirror", "ptp_gm"):
                ln = ln.replace(" ../src/%s.c" % m, "")
                ln = ln.replace(" " + OBJDIR + m + ".o.d", "")
                ln = re.sub(r" " + re.escape(OBJDIR + m + ".o") + r"(?!\.d)", "", ln)
        out.append(ln)
        i += 1
    if dropped != 4:
        sys.exit("dropped %d compile rules, expected 4 (two modules x two configurations)" % dropped)
    write_norm(mk, "".join(out), mk_nl)
    changes.append(mk.relative_to(ROOT))

    edit(DST_FW / (NEW_NAME + ".X") / "nbproject" / "configurations.xml", [
        ("      <itemPath>../src/port_mirror.h</itemPath>\n", ""),
        ("      <itemPath>../src/ptp_gm.h</itemPath>\n", ""),
        ("      <itemPath>../src/port_mirror.c</itemPath>\n", ""),
        ("      <itemPath>../src/ptp_gm.c</itemPath>\n", ""),
    ])

    # the bridge's hand patch inside generated driver code
    edit(CFG / "driver" / "lan865x" / "src" / "dynamic" / "drv_lan865x_api.c", [
        ("""    /* T1S->eth1 port mirror (SPAN) for Wireshark: this is the single eth0 egress
     * point, so every bridge-originated/forwarded frame passes here. The hook
     * (app.c) is a no-op unless "mirror" is on, and it only clones frames the
     * bridge itself originates (src MAC == eth0 MAC). Called before the mutex so
     * it never holds the LAN865x lock while transmitting on eth1/GMAC. */
    {
        extern void mirror_eth0_tx_hook(TCPIP_MAC_PACKET *txPkt);
        mirror_eth0_tx_hook(ptrPacket);
    }

""", "")])

    edit(SRC / "app.c", [
        ('#include "port_mirror.h"\n', ""),
        ('#include "ptp_gm.h"\n', ""),
        ("bool pktEth1Handler(TCPIP_NET_HANDLE hNet, struct _tag_TCPIP_MAC_PACKET* rxPkt, uint16_t frameType, const void* hParam);\n"
         "const void *MyEth1HandlerParam;\n", ""),
        ("    MIRROR_Initialize();\n", ""),
        ("    PTP_GM_Initialize();\n", ""),
        ("""            TCPIP_NET_HANDLE eth1_net_hd = TCPIP_STACK_IndexToNet(1);
            TCPIP_STACK_PacketHandlerRegister(eth1_net_hd, pktEth1Handler, MyEth1HandlerParam);
""", ""),
        ("""
            /* PTP grandmaster send cycle - see ptp_gm.c. Idle unless started. */
            PTP_GM_Tasks();
""", ""),
        ("""    /* Port mirror (SPAN) for Wireshark - see port_mirror.c. Checks the enable
     * flag and the own-MAC filter itself. */
    MIRROR_Eth0Rx(rxPkt);

""", ""),
        ("""bool pktEth1Handler(TCPIP_NET_HANDLE hNet, struct _tag_TCPIP_MAC_PACKET* rxPkt, uint16_t frameType, const void* hParam) {
    static uint32_t packet_counter = 0;

    packet_counter++;

    if (ipdump_mode == 2 || ipdump_mode == 3) {
        PKT_LOG_ENTRY log_e = {0};
        log_e.timestamp   = SYS_TIME_Counter64Get();
        log_e.pkt_counter = packet_counter;
        log_e.frame_type  = frameType;
        log_e.length      = rxPkt->pDSeg->segLen;
        log_e.iface       = 1u;
        log_e.log_type    = PKT_LOG_ETH1;
        memcpy(log_e.mac_src, &rxPkt->pDSeg->segLoad[6], 6u);
        PktLog_Write(&log_e, rxPkt->pDSeg->segLoad, rxPkt->pDSeg->segLen);
    }
    return false;
}

""", ""),
        ('    const char *ifNames[] = {"eth0", "eth1"};\n    int i;\n    for (i = 0; i < 2; i++) {\n',
         '    const char *ifNames[] = {"eth0"};      /* the follower has one interface */\n'
         '    int i;\n    for (i = 0; i < 1; i++) {\n'),
        ('    {"ipdump", (SYS_CMD_FNC) my_dump, ": dump rx ip packets (0:off 1:eth0 2:eth1 3:both)"},',
         '    {"ipdump", (SYS_CMD_FNC) my_dump, ": dump rx ip packets (0:off 1:eth0)"},'),
        ('    {"stats", (SYS_CMD_FNC) cmd_stats, ": show TX/RX counters for eth0 and eth1"},',
         '    {"stats", (SYS_CMD_FNC) cmd_stats, ": show TX/RX counters for eth0"},'),
        ("""    } else if (ipdump_mode == 2) {
        SYS_CONSOLE_PRINT("IP Layer Dump activated on eth1\\n\\r");
    } else if (ipdump_mode == 3) {
        SYS_CONSOLE_PRINT("IP Layer Dump activated on eth0 and eth1\\n\\r");
    } else {""", "    } else {"),
    ])

    edit(SRC / "noip_test.c", [
        ('#include "port_mirror.h"                                     /* MIRROR_RawTx() */\n', ""),
        ("""        /* The raw send above bypasses DRV_LAN865X_PacketTx() and therefore the
         * port mirror's TX hook, and the MAC bridge only floods what it RECEIVES
         * on a port - so without this call the frame is invisible on eth1 even
         * with "mirror 1". Respects that switch itself. */
        MIRROR_RawTx(s_frame, (uint16_t)sizeof(s_frame));
""", ""),
    ])
    edit(SRC / "noip_test.h", [
        ("""    Transmit also calls MIRROR_RawTx() from port_mirror.h after each frame, which
    is what makes these frames visible to Wireshark on eth1 while "mirror 1" is
    on. The raw driver path bypasses the mirror's normal TX hook, so without that
    call a capture stays empty and the mirror looks broken. Dropping the call is
    the only thing needed to make this module independent of port_mirror again.
""",
         """    This copy has no mirror call: the follower has a single interface, so there
    is nowhere to clone a frame to. On the bridge the same module calls
    MIRROR_RawTx() after each send, which is what makes its frames visible to a
    Wireshark PC on eth1.
"""),
    ])

    stage3_env()


def stage3_env():
    edit(SRC / "env.c", [
        ("#define ENV_MAGIC    0x4C414E45u   /* 'LANE' */\n"
         "#define ENV_VERSION  4u            /* v2 PLCA, v3 MACs, v4 PTP; an older record reads invalid -> re-seed */\n"
         "#define ENV_IF_CNT   2             /* [0] = eth0 (LAN865x/T1S), [1] = eth1 (GMAC/100BASE-T) */\n",
         "#define ENV_MAGIC    0x4C414E46u   /* 'LANF' - follower record; deliberately not readable as a bridge one */\n"
         "#define ENV_VERSION  1u            /* follower v1: one interface, no grandmaster keys */\n"
         "#define ENV_IF_CNT   1             /* [0] = eth0 (LAN865x/T1S) - the follower has no second interface */\n"),
        ("    uint32_t ptp_auto;             /* 1 = start the PTP grandmaster after boot        */\n"
         "    uint32_t ptp_ival;             /* PTP send interval in ms, also used by autostart  */\n", ""),
        ('#include "ptp_gm.h"                                          /* PTP_GM_ConfigureAutoStart() */\n', ""),
        ("/* Derive the per-board default MACs from the SAME54 serial: eth0 = OUI + serial[2..0],\n"
         " * eth1 = eth0 with the lowest byte +1. */\n"
         "static void env_derive_mac(uint8_t m0[6], uint8_t m1[6])\n{\n"
         "    uint32_t s = SAME54_SERIAL_WORD0;\n"
         "    m0[0] = ENV_OUI[0]; m0[1] = ENV_OUI[1]; m0[2] = ENV_OUI[2];\n"
         "    m0[3] = (uint8_t)(s >> 16); m0[4] = (uint8_t)(s >> 8); m0[5] = (uint8_t)s;\n"
         "    memcpy(m1, m0, 6);\n    m1[5] = (uint8_t)(m0[5] + 1u);\n}\n",
         "/* Derive the board's default MAC from the SAME54 serial: OUI + serial[2..0], so\n"
         " * every follower on the segment is unique with one firmware image. */\n"
         "static void env_derive_mac(uint8_t m0[6])\n{\n"
         "    uint32_t s = SAME54_SERIAL_WORD0;\n"
         "    m0[0] = ENV_OUI[0]; m0[1] = ENV_OUI[1]; m0[2] = ENV_OUI[2];\n"
         "    m0[3] = (uint8_t)(s >> 16); m0[4] = (uint8_t)(s >> 8); m0[5] = (uint8_t)s;\n}\n"),
        ("    env_derive_mac(e->mac[0], e->mac[1]);\n", "    env_derive_mac(e->mac[0]);\n"),
        ('    static const char *const dip[ENV_IF_CNT]   = { TCPIP_NETWORK_DEFAULT_IP_ADDRESS_IDX0, TCPIP_NETWORK_DEFAULT_IP_ADDRESS_IDX1 };\n'
         '    static const char *const dmask[ENV_IF_CNT] = { TCPIP_NETWORK_DEFAULT_IP_MASK_IDX0,    TCPIP_NETWORK_DEFAULT_IP_MASK_IDX1 };\n'
         '    static const char *const dgw[ENV_IF_CNT]   = { TCPIP_NETWORK_DEFAULT_GATEWAY_IDX0,    TCPIP_NETWORK_DEFAULT_GATEWAY_IDX1 };\n'
         '    static const char *const ddns[ENV_IF_CNT]  = { TCPIP_NETWORK_DEFAULT_DNS_IDX0,        TCPIP_NETWORK_DEFAULT_DNS_IDX1 };\n',
         '    static const char *const dip[ENV_IF_CNT]   = { TCPIP_NETWORK_DEFAULT_IP_ADDRESS_IDX0 };\n'
         '    static const char *const dmask[ENV_IF_CNT] = { TCPIP_NETWORK_DEFAULT_IP_MASK_IDX0 };\n'
         '    static const char *const dgw[ENV_IF_CNT]   = { TCPIP_NETWORK_DEFAULT_GATEWAY_IDX0 };\n'
         '    static const char *const ddns[ENV_IF_CNT]  = { TCPIP_NETWORK_DEFAULT_DNS_IDX0 };\n'),
        ("    /* PTP off by default: a bridge that streams Sync into a foreign network\n"
         "     * unasked is bad behaviour, and it would falsify the frame-counting test\n"
         "     * scripts (PTP_IMPLEMENTATION_PLAN.md 1.5). */\n"
         "    e->ptp_auto = 0u;\n"
         "    e->ptp_ival = (uint32_t)PTP_GM_INTERVAL_DEFAULT_MS;\n", ""),
        ('static const char *const ENV_IF[ENV_IF_CNT] = { "eth0", "eth1" };\n',
         'static const char *const ENV_IF[ENV_IF_CNT] = { "eth0" };\n'),
        ("    /* PTP grandmaster: hand over the persisted setting. Only the first call can\n"
         "     * arm the automatic start - a later saveenv/readenv adopts the interval but\n"
         "     * never starts sending on its own. The start itself happens in\n"
         "     * PTP_GM_Tasks(), where LAN865x register access is serviced. */\n"
         "    PTP_GM_ConfigureAutoStart(s_env.ptp_auto != 0u, s_env.ptp_ival);\n", ""),
        ('        env_mac_str(0, mb); SYS_CONSOLE_PRINT("  eth0  mac %s\\r\\n", mb);\n'
         '        env_mac_str(1, mb); SYS_CONSOLE_PRINT("  eth1  mac %s  (applied at boot)\\r\\n", mb);\n',
         '        env_mac_str(0, mb); SYS_CONSOLE_PRINT("  eth0  mac %s  (applied at boot)\\r\\n", mb);\n'),
        ('    SYS_CONSOLE_PRINT("  ptp   auto %lu  interval %lu ms  (grandmaster on eth0; auto applies at boot)\\r\\n",\n'
         '                      (unsigned long)s_env.ptp_auto, (unsigned long)s_env.ptp_ival);\n', ""),
        ('    if (!strcmp(key, "ip1"))   return &s_env.ip[1];\n'
         '    if (!strcmp(key, "mask1")) return &s_env.mask[1];\n'
         '    if (!strcmp(key, "gw1"))   return &s_env.gw[1];\n'
         '    if (!strcmp(key, "dns1"))  return &s_env.dns[1];\n', ""),
        ('                          "  IP keys:   ip0/mask0/gw0/dns0, ip1/mask1/gw1/dns1  (dotted-quad)\\r\\n"\n'
         '                          "  MAC keys:  mac0, mac1  (XX:XX:XX:XX:XX:XX; applies after reset)\\r\\n"\n'
         '                          "  PLCA keys: plca_id (0..254), plca_cnt (1..255)\\r\\n"\n'
         '                          "  PTP keys:  ptp_auto (0/1; applies after reset), ptp_ival (ms)\\r\\n");\n',
         '                          "  IP keys:   ip0/mask0/gw0/dns0  (dotted-quad)\\r\\n"\n'
         '                          "  MAC key:   mac0  (XX:XX:XX:XX:XX:XX; applies after reset)\\r\\n"\n'
         '                          "  PLCA keys: plca_id (0..254), plca_cnt (1..255)\\r\\n");\n'),
        ('    /* MAC keys (applied on next reset - the stack binds the MAC at init) */\n'
         '    if (!strcmp(argv[1], "mac0") || !strcmp(argv[1], "mac1")) {\n'
         '        int idx = (argv[1][3] == \'1\') ? 1 : 0;\n        uint8_t m[6];\n',
         '    /* MAC key (applied on next reset - the stack binds the MAC at init) */\n'
         '    if (!strcmp(argv[1], "mac0")) {\n'
         '        const int idx = 0;\n        uint8_t m[6];\n'),
        ("    /* PTP grandmaster keys. ptp_auto only takes effect at the next boot - the\n"
         "     * running state is what 'ptp start'/'ptp stop' control. */\n"
         '    if (!strcmp(argv[1], "ptp_auto") || !strcmp(argv[1], "ptp_ival")) {\n'
         "        unsigned long v = strtoul(argv[2], NULL, 0);\n"
         '        if (!strcmp(argv[1], "ptp_auto")) {\n'
         '            if (v > 1u) { SYS_CONSOLE_PRINT("setenv: ptp_auto is 0 or 1\\r\\n"); return; }\n'
         "            s_env.ptp_auto = (uint32_t)v;\n"
         '            SYS_CONSOLE_PRINT("setenv: ptp_auto = %lu (RAM only; \'saveenv\' to persist; applies after reset)\\r\\n", v);\n'
         "        } else {\n"
         "            if (v < (unsigned long)PTP_GM_INTERVAL_MIN_MS || v > (unsigned long)PTP_GM_INTERVAL_MAX_MS) {\n"
         '                SYS_CONSOLE_PRINT("setenv: ptp_ival range %u..%u ms\\r\\n",\n'
         "                                  (unsigned)PTP_GM_INTERVAL_MIN_MS, (unsigned)PTP_GM_INTERVAL_MAX_MS);\n"
         "                return;\n            }\n"
         "            s_env.ptp_ival = (uint32_t)v;\n"
         '            SYS_CONSOLE_PRINT("setenv: ptp_ival = %lu ms (RAM only; \'saveenv\' to persist)\\r\\n", v);\n'
         "        }\n        return;\n    }\n", ""),
        ('    {"setenv",   (SYS_CMD_FNC)cmd_setenv,   ": setenv <key> <val>  (ip0../dns1, mac0/mac1, plca_id, plca_cnt, ptp_auto, ptp_ival)"},',
         '    {"setenv",   (SYS_CMD_FNC)cmd_setenv,   ": setenv <key> <val>  (ip0/mask0/gw0/dns0, mac0, plca_id, plca_cnt)"},'),
        (" * env.c - persistent network config (\"environment\") on the Emulated EEPROM.\n",
         " * env.c - persistent config (\"environment\") on the Emulated EEPROM.\n *\n"
         " * Follower variant: one interface (eth0 = LAN865x/T1S) and no grandmaster keys.\n"),
    ])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="delete an existing follower/ first")
    args = ap.parse_args()

    if DST.exists():
        if not args.force:
            sys.exit("%s exists. It is the real project - pass --force only if you mean to "
                     "throw it away and re-derive." % DST)
        print("removing %s" % DST)
        shutil.rmtree(DST)

    stage1_copy()
    stage2_single_interface()
    stage3_strip_bridge_role()

    print("\nderived %s" % DST)
    print("files changed after the copy: %d" % len(changes))
    print("\nnext: follower\\build.bat, then follower\\flash.bat (boards.json decides the board)")
    print("note: the follower's eth0 default IP is the same as the bridge's - give it its own")
    print("      address on the segment with 'setenv ip0 <addr>' + 'saveenv'.")


if __name__ == "__main__":
    main()
