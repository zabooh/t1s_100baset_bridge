# Bridge Status & Configuration GUI

A Python tkinter-based GUI for managing T1S/100BASE-T Bridge firmware parameters and LAN8651 register access.

## Quick Start

### Launch the GUI

**Option 1: Batch file (Windows)**
```bash
run_gui.bat
```

**Option 2: Direct Python**
```bash
python bridge_gui.py
```

### Connect to Device

1. Select correct COM port (top left)
2. Click **🟢 Connect** button
3. Watch status: indicator changes from 🔴 Offline → 🟢 Online
4. Command Output window shows: `[HH:MM:SS] ✓ Connected to device`
5. Ready to read/write parameters and registers

### Disconnect

Click **🔴 Disconnect** button to close connection (indicator changes to 🔴 Offline)

## Requirements

- Python 3.7 or higher
- tkinter (included with Python on Windows)
- The `cli.py` tool in the same directory
- Connection to the bridge via COM port (EDBG probe)

## Features

### Connection Management (Top Bar)

- **COM Port Selection**: Dropdown shows only available ports (auto-detected)
- **🔄 Refresh Ports**: Update port list (e.g., after plugging in EDBG probe)
- **Update COM Port**: Save selected port to config
- **🟢 Connect**: Test connection by pinging device (status indicator updates)
- **🔴 Disconnect**: Close connection (status indicator updates)
- **Connection Indicator**: Green circle = Online, Red circle = Offline
- **Status Label**: Real-time feedback (top right)

### Bridge Parameters Tab

**Two-pane layout:**

**Left Pane - Configuration Parameters:**
- **Individual Read/Write**: Read or write single parameters
- **Read Environment**: Fetch all bridge configuration from device
- **Write All**: Apply all parameter changes to device
- **Save to JSON**: Persist current configuration
- **Open from JSON**: Reload configuration from file

Managed parameters:
- IP addresses (`ip_eth0`, `ip_eth1`)
- MAC addresses (`mac_eth0`, `mac_eth1`)
- PLCA settings (`plca_id`, `plca_cnt`)

**Right Pane - Quick Commands:**
- **Environment Section**: Read Environment, Write Environment, Save/Open JSON
- **Device Section**: Mirror Enable/Disable, Read Stats, Memory Info
- **Command Output**: Real-time output with timestamps, Clear button to reset

### LAN8651 Registers Tab

Organized into **multiple sub-tabs by category** for better organization:

#### Sub-tabs (Categories):
- **Test Mode**: T1STSTCTL, T1SPMACTL, T1SPMASTS (IEEE test mode control)
- **PLCA**: PLCA_CTRL0, PLCA_CTRL1, PLCA_STATUS (Power Line Communication)
- **MAC**: MAC timing and status registers
- **Status**: OA_STATUS0, OA_STATUS1, PHY_PCS_Status
- **Diagnostics**: Device ID and legacy registers

#### Features:
- **Bulk Read**: Read all registers from all categories at once
- **Bulk Write**: Write all registers at once
- **Individual Read/Write**: Access single registers with automatic readback
- **Save/Open JSON**: Persist register values by category

Pre-configured registers can be expanded in `bridge_config.json`:
```json
"registers": {
  "Test Mode": {
    "0x000308FB": "T1STSTCTL (Test Mode Control)",
    "0x000308F9": "T1SPMACTL (PMA Control)"
  },
  "PLCA": {
    "0x0004CA02": "PLCA_CTRL1 (Node ID / Count)"
  }
}
```

### Test Modes Tab

IEEE 802.3 test modes and diagnostics:

**IEEE 802.3 Test Modes Section:**
- **Apply Test Mode**: Set mode 0-4 with optional auto-revert timer
  - Mode 0: Normal operation
  - Mode 1-4: Various transmitter tests (see LAN8651_TEST_MODES.md)
  - ⚠️ Warning: Link is disconnected during test modes
  
- **Read Current Mode**: Query current test mode state from `0x000308FB`

**Automated Testing Section:**
- **Run test_lan8651.py**: Execute full test suite with automatic verification
  - Tests mode set/readback
  - Verifies traffic stops during test
  - Confirms traffic resumes after revert

**Test Mode Reference Section:**
Quick reference table showing:
- Mode number and name
- What to measure (Oscilloscope, Spectrum Analyzer, etc.)
- Key parameters for each mode
- Link to detailed LAN8651_TEST_MODES.md documentation

### Terminal Tab

Live serial terminal (115200 8N1) for interactive device communication:

**Controls:**
- **🟢 Connect Terminal**: Open serial port and start reading
- **🔴 Disconnect Terminal**: Close connection
- **Clear**: Clear terminal display
- **Status Indicator**: Green = Online, Red = Offline

**Features:**
- **Serial Output**: Real-time display of device output (latin-1 encoding)
- **Command Input**: Type commands and press Enter (or click Send)
- **Keyboard Input**: 
  - Regular keys send as-is (ASCII)
  - Enter sends CR
  - Backspace sends BS (0x08)
  - Tab sends TAB
- **Ctrl+C**: Send interrupt byte (0x03) or copy selection
- **Ctrl+V / Right-click**: Paste from clipboard (line by line)
- **Escape sequences**: CSI sequences (ESC[) are handled (unsupported ones discarded)
- **Line editor**: Supports Backspace, left/right/up/down arrows

**Example Workflow:**
1. Go to **Terminal** tab
2. Click **🟢 Connect Terminal**
3. Type command: `stats`
4. Press Enter → device echoes and responds
5. See output in terminal window
6. Click **🔴 Disconnect Terminal** when done

### Help Tab

Quick reference, keyboard shortcuts, and usage guide for all tabs.

## Configuration File

Settings are stored in `bridge_config.json` in the same directory as the script.

### Example Structure

```json
{
  "comport": "COM8",
  "baudrate": 115200,
  "bridge": {
    "ip_eth0": "192.168.0.200",
    "ip_eth1": "192.168.0.210",
    "mac_eth0": "00:04:25:1A:00:00",
    "mac_eth1": "00:04:25:1A:00:01",
    "plca_id": 0,
    "plca_cnt": 8
  },
  "registers": {
    "Test Mode": {
      "0x000308FB": "T1STSTCTL (Test Mode Control)",
      "0x000308F9": "T1SPMACTL (PMA Control)",
      "0x000308FA": "T1SPMASTS (PMA Status)"
    },
    "PLCA": {
      "0x0004CA02": "PLCA_CTRL1 (Node ID / Count)",
      "0x0004CA03": "PLCA_CTRL0 (Enable / Burst)"
    },
    "MAC": {
      "0x00010077": "MAC_TI (MAC Time Interval)"
    },
    "Status": {
      "0x00000008": "OA_STATUS0 (Chip Status)"
    },
    "Diagnostics": {
      "0x00030001": "PMA_STATUS1 (Legacy)"
    }
  }
}
```

## Workflow Examples

### Quick Device Status Check

1. Select correct COM port (e.g., COM8) at top
2. Go to **Bridge Parameters** tab
3. In right pane, click **Read Stats** → output appears in "Command Output" window
4. Click **Read Environment** (Environment section) to fetch configuration
5. View current state in left pane fields

### Apply and Test a Test Mode

1. Go to **Test Modes** tab
2. Select mode from spinner (0 = normal, 1-4 = test)
3. Optionally enter auto-revert timeout (e.g., 30 seconds)
4. Click **Apply Test Mode**
5. Wait for command to complete (status bar shows "Command OK")
6. ✅ Verify in Command Output (see Bridge Parameters tab right pane)
7. Go to **LAN8651 Registers** → **Test Mode** sub-tab
8. Click **Read** on `0x000308FB` to see register value confirm mode

### Mirror Traffic to eth1

1. Go to **Bridge Parameters** tab
2. Right pane, click **Mirror: Enable**
3. Output appears in "Command Output" window
4. Wireshark can now see T1S traffic on eth1 port
5. To disable: click **Mirror: Disable**

### Run Automatic Test Suite

1. Go to **Test Modes** tab
2. Click **Run test_lan8651.py**
3. Script runs all 4 test modes, verifies each one
4. Output from test script appears in Bridge Parameters tab
5. Exitcode tells you if all tests passed

### Save/Restore Configuration

**Save:**
1. Go to **Bridge Parameters** tab, edit fields as needed
2. Click **Save to JSON** (saves bridge params)
3. Go to **LAN8651 Registers**, edit register values
4. Click **Save to JSON** (saves registers by category)
5. Configuration persisted in `bridge_config.json`

**Restore:**
1. Go to **Bridge Parameters** tab, click **Open from JSON**
2. All fields populate from saved file
3. Click **Write All** to apply to device
4. Go to **LAN8651 Registers**, click **Open from JSON**
5. Click **Bulk Register Write All** to apply to device

### Examine Multiple Registers Across Categories

1. Go to **LAN8651 Registers** tab (shown as sub-tabs)
2. Each category has its own tab: Test Mode, PLCA, MAC, Status, Diagnostics
3. Individual **Read** on any register to fetch current value
4. Or click **Bulk Register Read All** (left pane, bottom) to read all categories at once
5. Click **Save to JSON** to store all values for comparison later

### Interactive Terminal Session

1. Go to **Terminal** tab
2. Click **🟢 Connect Terminal** (status turns green)
3. Type a command: `stats`
4. Press Enter → command echoes and device responds
5. Keep terminal window in focus for keyboard input
6. Use Ctrl+V to paste multi-line scripts (sends line-by-line with auto-delay)
7. Use Ctrl+C to send interrupt (Ctrl+Break) or copy selection
8. Click **Clear** to reset terminal display
9. When done, click **🔴 Disconnect Terminal**

## Threading & UI

All long-running operations (register reads, test scripts) run in background threads. The UI stays responsive:

- **Status label** (top right) shows current operation
- **Green "OK"** message = command succeeded
- **Red error message** = command failed (see dialog box for details)
- **"Ready"** message indicates idle state
- **Command Output** (Bridge Parameters tab, right pane) shows real-time output from all commands with timestamps
  - Click **Clear** button to reset output history
- **UI never freezes** — you can read registers while a test script runs

## Error Handling

- **COM port not responding**: Check connection, verify correct port selected
- **"cli.py not found"**: GUI must be in same directory as cli.py
- **Timeout errors**: Device may be in different app state, try "Read Environment" first
- **Register readback failed**: Device may not support register, verify address

## Troubleshooting

### GUI won't start

```bash
python -m tkinter  # Test tkinter installation
python bridge_gui.py -v  # Run with verbose output
```

### Commands timing out

- Bridge may be in non-IDLE state (check `stats` command)
- Increase timeout in `cli.py` if needed
- Verify EDBG COM port is correct

### Register values not updating

1. Check status bar for error message
2. Verify register address is correct (MMS encoding)
3. Try individual **Read** button to see error output
4. Check device is in IDLE state: `python cli.py --port COM8 --read 1 "stats"`

## Adding Custom Parameters or Registers

### Add Bridge Parameter

Edit `bridge_config.json`:
```json
"bridge": {
  "my_new_param": "default_value"
}
```

The GUI will automatically create a read/write field.

### Add LAN8651 Register

Edit `bridge_config.json` to add a register to an existing category:
```json
"registers": {
  "Test Mode": {
    "0xMMMMOOOO": "Description of Register"
  }
}
```

Or create a new category:
```json
"registers": {
  "My Custom Registers": {
    "0xMMMMOOOO": "Description",
    "0xAAAABBBB": "Another Register"
  }
}
```

Each category will automatically get its own sub-tab in the GUI.

Format: `0x` + 4 hex digits (MMS) + 4 hex digits (offset)

**Common MMS values:**
- `0x0000` - OA Standard (Status, Config)
- `0x0001` - MAC (Timestamps, Timing)
- `0x0002` - PHY PCS (Clause 45)
- `0x0003` - PHY PMA/PMD (Test Modes)
- `0x0004` - PHY Vendor-Specific (PLCA, SQI, CFD)
- `0x000A` - Misc (Events, 1PPS, Device ID)

## Command Line Reference

The GUI calls `cli.py` with this pattern:

```bash
python cli.py --port COM8 --read 1 "<command>"
```

Common commands executed by GUI:

| Button | Command |
|--------|---------|
| Read Register | `lan_read <addr>` |
| Write Register | `lan_write <addr> <value>` |
| Test Mode | `testmode <mode> [seconds]` |
| Mirror | `mirror [0\|1]` |
| Stats | `stats` |
| PLCA Node | `plca_node [id]` |
| Read Env | `showenv` |
| Set Env | `setenv <key> <value>` |

See project README.md and LAN8651_TEST_MODES.md for full command reference.

## Architecture

```
bridge_gui.py
├── BridgeGUI (main window)
├── CLIRunner (subprocess manager)
├── result_queue (thread-safe communication)
└── Threading workers (async command execution)
```

Key classes:

- **CLIRunner**: Manages subprocess calls to cli.py, parses output
- **BridgeGUI**: Main window with tabs, fields, buttons
- **Threading**: Background workers prevent UI freeze on long operations

## Performance Notes

- Register bulk read: ~1 sec per 10 registers (with 100ms delays)
- Test script: ~30 sec typical (varies with test)
- Status updates: Real-time via queue system
- Memory: <50 MB typical usage

## Support

For issues with:
- **Bridge/firmware**: See project README.md
- **Test modes**: See LAN8651_TEST_MODES.md
- **Registers**: See LAN8651 datasheet
- **CLI tool**: Check cli.py source and --help

## License

Part of T1S 100BASE-T Bridge project.
