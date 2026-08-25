#!/usr/bin/env python3
"""
Bridge Status & Configuration GUI

Bedient die T1S/100BASE-T-Bridge über den EDBG-COM-Port (115200 8N1):
Bridge-Parameter, LAN8651-Register, IEEE-Testmodi und ein Terminal.

Standalone. Gebraucht werden nur pyserial und bridge_config.json (liegt
daneben). Kein cli.py, kein test_lan8651.py -- die öffnen den COM-Port selbst
und kollidieren mit der Verbindung dieser GUI, weil der Port unter Windows
exklusiv ist. Alle Kommandos laufen über den einen offenen Link.
"""

import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext
import json
import os
import threading
import re
from pathlib import Path
from typing import Dict, Optional, List
import queue
import time
import winreg

try:
    import serial
except ImportError:
    serial = None

# Configuration file path
CONFIG_FILE = Path(__file__).parent / "bridge_config.json"

# Default configuration
# Vorgaben, falls bridge_config.json fehlt. Die Registerkarte kommt dann NICHT
# mit -- sie stammt aus dem Datenblatt (LAN8650-1-Data-Sheet-60001734.pdf,
# Kapitel 11, 182 Register) und steht ausschliesslich in bridge_config.json.
# Frueher standen hier von Hand ergaenzte Adressen; vier davon existierten gar
# nicht und zwei trugen den falschen Namen, siehe CLAUDE.md Abschnitt 6.
DEFAULT_CONFIG = {
    "comport": "COM8",
    "baudrate": 115200,
    "bridge": {
        "ip_eth0": "192.168.0.200",
        "ip_eth1": "192.168.0.210",
        "mac_eth0": "00:04:25:1A:00:00",
        "mac_eth1": "00:04:25:1A:00:01",
        "plca_id": 0,
        "plca_cnt": 8,
    },
    "registers": {},
}

# Terminal-spezifische Konstanten (aus gui_term.py)
KEYSYM_BYTES = {
    "Return": b"\r",
    "KP_Enter": b"\r",
    "BackSpace": b"\x08",
    "Delete": b"\x1b[3~",
    "Left": b"\x1b[D",
    "Right": b"\x1b[C",
    "Up": b"\x1b[A",
    "Down": b"\x1b[B",
    "Home": b"\x1b[H",
    "End": b"\x1b[F",
    "Escape": b"\x1b",
    "Tab": b"\t",
}

IGNORED_KEYSYMS = {
    "Shift_L", "Shift_R", "Control_L", "Control_R", "Alt_L", "Alt_R",
    "Caps_Lock", "Num_Lock", "Scroll_Lock", "Win_L", "Win_R", "App", "Insert",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
}

CONTROL_BIT = 0x0004
POLL_MS = 30
BLINK_MS = 500
MAX_VIEW_LINES = 1000
BAUD = 115200


class Screen:
    """Byte-to-text converter (aus gui_term.py)"""

    def __init__(self, max_lines=MAX_VIEW_LINES):
        self.max_lines = max_lines
        self.lines = []
        self.total = 0
        self.cur = ""
        self.col = 0
        self._esc = None

    def feed(self, data):
        """Feed raw bytes and convert to screen state"""
        for b in data:
            if self._esc is not None:
                self._esc.append(b)
                if len(self._esc) == 1:
                    if b not in (0x5B, 0x4F):
                        self._esc = None
                    continue
                if 0x40 <= b <= 0x7E:
                    self._sequence(bytes(self._esc))
                    self._esc = None
                continue
            if b == 0x1B:
                self._esc = bytearray()
            elif b == 0x0D:
                self.col = 0
            elif b == 0x0A:
                self._newline()
            elif b == 0x08:
                self.col = max(0, self.col - 1)
            elif b == 0x09:
                self._put(" " * (8 - (self.col % 8)))
            elif 0x20 <= b <= 0xFF and b != 0x7F:
                self._put(chr(b))

    def _sequence(self, seq):
        """Handle escape sequences"""
        final = seq[-1:]
        body = seq[1:-1]
        if final == b"K":
            if body in (b"", b"0"):
                self.cur = self.cur[:self.col]
            elif body == b"1":
                self.cur = " " * self.col + self.cur[self.col:]
            elif body == b"2":
                self.cur = ""
                self.col = 0

    def _put(self, s):
        if self.col > len(self.cur):
            self.cur += " " * (self.col - len(self.cur))
        self.cur = self.cur[:self.col] + s + self.cur[self.col + len(s):]
        self.col += len(s)

    def _newline(self):
        self.lines.append(self.cur)
        self.total += 1
        self.cur = ""
        self.col = 0
        if len(self.lines) > self.max_lines:
            del self.lines[:len(self.lines) - self.max_lines]

    def text(self):
        return "".join(line + "\n" for line in self.lines) + self.cur


class Link:
    """Serial connection mit Reader-Thread (aus gui_term.py)"""

    def __init__(self, port, q, baud=BAUD):
        self.port = port
        self.baud = baud
        self.q = q
        self.ser = None
        self.stop = threading.Event()
        self.thread = None

    def open(self):
        if serial is None:
            raise ImportError("pyserial nicht installiert")
        self.ser = serial.Serial(self.port, self.baud, timeout=0.05)
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        while not self.stop.is_set():
            try:
                n = self.ser.in_waiting
                data = self.ser.read(n if n else 1)
            except Exception as error:
                if not self.stop.is_set():
                    self.q.put((self.port, "lost", str(error)))
                return
            if data:
                self.q.put((self.port, "data", data))

    def write(self, data):
        if self.ser is None:
            raise OSError("nicht verbunden")
        self.ser.write(data)

    def close(self):
        self.stop.set()
        if self.thread is not None:
            self.thread.join(timeout=0.5)
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None


def get_available_com_ports() -> List[str]:
    """
    Detect available COM ports on Windows.

    First tries pyserial, falls back to Windows Registry.
    """
    # Try pyserial first (most reliable)
    try:
        from serial.tools import list_ports
        ports = [port.device for port in list_ports.comports()]
        return sorted(ports) if ports else []
    except ImportError:
        pass

    # Fallback: Windows Registry
    try:
        com_ports = []
        reg_path = r"HARDWARE\DEVICEMAP\SERIALCOMM"
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, reg_path) as key:
            for i in range(winreg.QueryInfoKey(key)[1]):
                name, value, _ = winreg.EnumValue(key, i)
                if value.startswith("COM"):
                    com_ports.append(value)
        return sorted(com_ports) if com_ports else []
    except Exception:
        pass

    # Last resort: check common ports
    common_ports = ["COM1", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9"]
    available = []
    for port in common_ports:
        try:
            import serial
            s = serial.Serial(port, timeout=0)
            s.close()
            available.append(port)
        except Exception:
            pass
    return available if available else common_ports


class ResponseParser:
    """Wertet die Antwortzeilen des Geräts aus.

    Hier lief früher ein Unterprozess auf cli.py. Das ging nicht: cli.py öffnet
    den COM-Port selbst (cli.py:47), und der ist unter Windows exklusiv -- solange
    die GUI verbunden ist, bekommt der Unterprozess "Zugriff verweigert". Alle
    Kommandos laufen deshalb über den bereits offenen Link
    (BridgeGUI.send_command_via_link); übrig bleibt das Parsen.
    """

    def parse_register_read(self, output: str) -> Optional[str]:
        """Wert aus 'LAN865X Read OK: Addr=... Value=...' ziehen."""
        match = re.search(r'Value=0x([0-9A-Fa-f]+)', output)
        if match:
            return "0x" + match.group(1)
        return None


class BridgeGUI:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Bridge Status & Configuration")
        # 1000 px was enough until the bulk buttons moved into the top bar; that row
        # needs 1162 px (measured) and pack() does not wrap - it silently cuts off
        # whatever is furthest right. The position is set too: on the 1280 px screen
        # here Windows placed the wider window at x=78, which pushed "Open from JSON"
        # off the edge - the buttons were there, just not reachable.
        self.root.geometry(self._fitting_geometry(1220, 700))
        self.root.minsize(1180, 500)

        self.config = self.load_config()
        self.cli = ResponseParser()
        self.result_queue = queue.Queue()
        self.connected = False
        self.port_link: Optional[Link] = None  # Globale Verbindung für CLI + Terminal

        # Kommando-Antworten laufen über eine EIGENE Queue. Sonst konkurrieren
        # terminal_process_queue() (Main-Thread, alle 30 ms) und der Worker-Thread
        # um dieselben Chunks, und die Antwort kommt zerrissen an -> leere Felder.
        self.cmd_response_q = queue.Queue()
        self.cmd_pending = threading.Event()
        self.cmd_lock = threading.Lock()

        # Validate saved COM port is still available
        available = get_available_com_ports()
        saved_port = self.config.get("comport", "COM8")
        if available and saved_port not in available:
            self.config["comport"] = available[0]
            self.save_config()

        self.setup_ui()

        # Auto-refresh COM ports on startup
        if available:
            self.set_status(f"Ready ({len(available)} port(s) available)")
        else:
            self.set_error_status("No COM ports detected")

        self.update_connection_indicator()

        # Start blink timer for terminal cursor
        self.root.after(BLINK_MS, self._blink_loop)

        self.process_queue()

    def _fitting_geometry(self, width: int, height: int) -> str:
        """Geometry string for a window of this size, clamped onto the visible screen.

        Shrinks the request if the screen is smaller and places the window so the right
        edge stays on screen - a window that is merely wide is a nuisance, one that hangs
        over the edge hides controls without any hint that they exist.
        """
        screen_w = self.root.winfo_screenwidth()
        screen_h = self.root.winfo_screenheight()
        width = min(width, screen_w - 20)
        height = min(height, screen_h - 80)
        x = max(0, (screen_w - width) // 2)
        y = max(0, min(40, (screen_h - height) // 2))
        return f"{width}x{height}+{x}+{y}"

    def load_config(self) -> dict:
        """Load configuration from JSON or create default.

        encoding="utf-8" is not optional here. save_config() writes UTF-8; without an
        explicit encoding this read takes the Windows default (cp1252), so the three
        bytes of "'" come back as three separate characters and the next save writes
        THOSE as UTF-8. The damage therefore compounds with every round trip -
        "Manufacturer's" turned into "Manufacturerâ€™s" and then into
        "ManufacturerÃ¢â‚¬â„¢s" - and nothing ever reports an error, because every
        intermediate file is valid JSON.
        """
        if CONFIG_FILE.exists():
            with open(CONFIG_FILE, 'r', encoding="utf-8") as f:
                return json.load(f)
        return DEFAULT_CONFIG.copy()

    def save_config(self):
        """Konfiguration schreiben -- erst serialisieren, dann ersetzen.

        open(..., 'w') leert die Datei beim Öffnen; scheitert json.dump danach,
        ist die Registerkarte weg. Deshalb zuerst in Bytes wandeln (ein Fehler
        fliegt dann, bevor irgendetwas angefasst wird), in eine Nachbardatei
        schreiben und erst zum Schluss über os.replace austauschen.
        """
        data = json.dumps(self.config, indent=2, ensure_ascii=False).encode("utf-8")
        tmp = CONFIG_FILE.with_suffix(".json.tmp")
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, CONFIG_FILE)

    def setup_ui(self):
        """Build the main UI"""
        # Top frame: COM port selection
        top_frame = ttk.Frame(self.root)
        top_frame.pack(fill=tk.X, padx=5, pady=5)

        ttk.Label(top_frame, text="COM Port:").pack(side=tk.LEFT)
        self.comport_var = tk.StringVar(value=self.config.get("comport", "COM8"))

        # Get available COM ports
        available_ports = get_available_com_ports()
        self.comport_combo = ttk.Combobox(
            top_frame,
            textvariable=self.comport_var,
            values=available_ports,
            width=10,
            state="readonly"
        )
        self.comport_combo.pack(side=tk.LEFT, padx=5)

        # Refresh COM ports button
        ttk.Button(top_frame, text="🔄 Refresh Ports", command=self.refresh_com_ports).pack(side=tk.LEFT, padx=2)

        ttk.Button(top_frame, text="Update COM Port", command=self.update_comport).pack(side=tk.LEFT, padx=2)

        # Connect/Disconnect buttons
        ttk.Button(top_frame, text="🟢 Connect", command=self.connect_device).pack(side=tk.LEFT, padx=2)
        ttk.Button(top_frame, text="🔴 Disconnect", command=self.disconnect_device).pack(side=tk.LEFT, padx=2)

        # Register bulk actions. They live up here rather than at the bottom of the
        # register tab because they are what one reaches for while watching the
        # registers, and the tab scrolls - a button below the scroll area is off screen
        # exactly when it is wanted.
        ttk.Separator(top_frame, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=6)
        ttk.Button(top_frame, text="🔄 Bulk Read All", command=self.bulk_read_registers).pack(side=tk.LEFT, padx=2)
        ttk.Button(top_frame, text="💾 Bulk Write All", command=self.bulk_write_registers).pack(side=tk.LEFT, padx=2)
        ttk.Button(top_frame, text="Save to JSON", command=self.save_registers_json).pack(side=tk.LEFT, padx=2)
        ttk.Button(top_frame, text="Open from JSON", command=self.load_registers_json).pack(side=tk.LEFT, padx=2)

        # Connection indicator
        self.connection_frame = ttk.Frame(top_frame)
        self.connection_frame.pack(side=tk.RIGHT, padx=10)

        self.connection_indicator = tk.Canvas(self.connection_frame, width=15, height=15, bg="white", highlightthickness=1)
        self.connection_indicator.pack(side=tk.LEFT)

        self.connection_label = ttk.Label(self.connection_frame, text="Offline", foreground="red")
        self.connection_label.pack(side=tk.LEFT, padx=5)

        # Status label
        self.status_label = ttk.Label(top_frame, text="Ready", foreground="blue")
        self.status_label.pack(side=tk.RIGHT, padx=5)

        # Notebook (tabs)
        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        self.create_bridge_tab()
        self.create_registers_tab()
        self.create_testmodes_tab()
        self.create_terminal_tab()
        self.create_about_tab()

    def create_bridge_tab(self):
        """Create Bridge Parameters tab"""
        frame = ttk.Frame(self.notebook)
        self.notebook.add(frame, text="Bridge Parameters")

        # Fields dictionary
        self.bridge_fields: Dict[str, tk.StringVar] = {}

        # Main paned window: parameters on left, commands/output on right
        paned = ttk.PanedWindow(frame, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # LEFT SIDE: Bridge Parameters
        left_frame = ttk.LabelFrame(paned, text="Configuration Parameters", padding=5)
        paned.add(left_frame, weight=1)

        # Scrollable frame for parameters
        canvas = tk.Canvas(left_frame)
        scrollbar = ttk.Scrollbar(left_frame, orient="vertical", command=canvas.yview)
        scrollable_frame = ttk.Frame(canvas)

        scrollable_frame.bind(
            "<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all"))
        )

        canvas.create_window((0, 0), window=scrollable_frame, anchor="nw")
        canvas.configure(yscrollcommand=scrollbar.set)

        # Create fields for bridge config
        bridge_cfg = self.config.get("bridge", DEFAULT_CONFIG["bridge"])
        for key, value in bridge_cfg.items():
            self.bridge_fields[key] = tk.StringVar(value=str(value))
            row = ttk.Frame(scrollable_frame)
            row.pack(fill=tk.X, padx=5, pady=2)

            ttk.Label(row, text=key + ":", width=15).pack(side=tk.LEFT)
            ttk.Entry(row, textvariable=self.bridge_fields[key], width=30).pack(side=tk.LEFT, padx=5)
            ttk.Button(row, text="Read", width=5, command=lambda k=key: self.read_bridge_field(k)).pack(side=tk.LEFT, padx=1)
            ttk.Button(row, text="Write", width=5, command=lambda k=key: self.write_bridge_field(k)).pack(side=tk.LEFT, padx=1)

        canvas.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        # RIGHT SIDE: Quick Commands
        right_frame = ttk.LabelFrame(paned, text="Quick Commands", padding=5)
        paned.add(right_frame, weight=1)

        # Parameter buttons
        param_btn_frame = ttk.LabelFrame(right_frame, text="Parameters", padding=5)
        param_btn_frame.pack(fill=tk.X, padx=5, pady=5)

        ttk.Button(param_btn_frame, text="Read All", command=self.read_all_bridge, width=15).pack(fill=tk.X, padx=2, pady=2)
        ttk.Button(param_btn_frame, text="Write All", command=self.write_all_bridge, width=15).pack(fill=tk.X, padx=2, pady=2)
        ttk.Button(param_btn_frame, text="Save to JSON", command=self.save_bridge_json, width=15).pack(fill=tk.X, padx=2, pady=2)
        ttk.Button(param_btn_frame, text="Open from JSON", command=self.load_bridge_json, width=15).pack(fill=tk.X, padx=2, pady=2)

        # Device commands
        device_btn_frame = ttk.LabelFrame(right_frame, text="Device", padding=5)
        device_btn_frame.pack(fill=tk.X, padx=5, pady=5)

        ttk.Button(device_btn_frame, text="Mirror: Enable", command=lambda: self.run_async_cmd("mirror 1"), width=15).pack(fill=tk.X, padx=2, pady=2)
        ttk.Button(device_btn_frame, text="Mirror: Disable", command=lambda: self.run_async_cmd("mirror 0"), width=15).pack(fill=tk.X, padx=2, pady=2)
        ttk.Button(device_btn_frame, text="Read Stats", command=lambda: self.run_async_cmd("stats"), width=15).pack(fill=tk.X, padx=2, pady=2)
        ttk.Button(device_btn_frame, text="Memory Info", command=lambda: self.run_async_cmd("meminfo"), width=15).pack(fill=tk.X, padx=2, pady=2)

        # Output frame
        output_frame = ttk.LabelFrame(right_frame, text="Command Output", padding=5)
        output_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # Output controls
        output_ctrl_frame = ttk.Frame(output_frame)
        output_ctrl_frame.pack(fill=tk.X, pady=2)
        ttk.Button(output_ctrl_frame, text="Clear", command=self.clear_bridge_output, width=10).pack(side=tk.LEFT)

        self.bridge_output = tk.Text(output_frame, height=20, width=40, state=tk.DISABLED, wrap=tk.WORD)
        self.bridge_output.pack(fill=tk.BOTH, expand=True)

        scrollbar_out = ttk.Scrollbar(self.bridge_output)
        scrollbar_out.pack(side=tk.RIGHT, fill=tk.Y)
        self.bridge_output.config(yscrollcommand=scrollbar_out.set)

    def create_registers_tab(self):
        """Create LAN8651 Registers tab with categories"""
        frame = ttk.Frame(self.notebook)
        self.notebook.add(frame, text="LAN8651 Registers")

        self.register_fields: Dict[str, tk.StringVar] = {}
        self.register_categories: Dict[str, List[str]] = {}
        # Registerkarte (Name/Beschreibung/Bitfelder je Adresse) im Speicher halten.
        # Ohne die schrieb save_registers_json nur {Adresse: Wert} zurück und hat
        # die aus dem Datenblatt erzeugte Karte beim ersten Speichern vernichtet.
        self.register_meta: Dict[str, dict] = {}

        # Create sub-notebook for register categories
        reg_notebook = ttk.Notebook(frame)
        reg_notebook.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # Get registers grouped by category
        registers = self.config.get("registers", DEFAULT_CONFIG["registers"])

        # If old flat format, convert it
        if registers and isinstance(next(iter(registers.values())), str):
            # Old format: convert to new format
            registers = {"All Registers": registers}

        # Create a tab for each category
        for category, regs in registers.items():
            self.register_categories[category] = list(regs.keys())
            category_frame = ttk.Frame(reg_notebook)
            reg_notebook.add(category_frame, text=category)

            # Scrollbarer Bereich. Die Bindungen MÜSSEN das Canvas dieses
            # Durchlaufs festhalten (Default-Argument): ein `lambda e: canvas...`
            # greift auf die Schleifenvariable zu, und die zeigt nach dem letzten
            # Durchlauf für ALLE Tabs auf dasselbe, zuletzt erzeugte Canvas --
            # dann bekommt nur der letzte Tab eine gültige scrollregion und alle
            # anderen lassen sich nicht über die sichtbare Höhe hinaus scrollen.
            canvas = tk.Canvas(category_frame, highlightthickness=0)
            scrollbar = ttk.Scrollbar(category_frame, orient="vertical", command=canvas.yview)
            scrollable_frame = ttk.Frame(canvas)
            inner_id = canvas.create_window((0, 0), window=scrollable_frame, anchor="nw")
            canvas.configure(yscrollcommand=scrollbar.set)

            def _on_content(event, cv=canvas):
                cv.configure(scrollregion=cv.bbox("all"))

            def _on_canvas(event, cv=canvas, wid=inner_id):
                # Inhalt auf die volle Canvasbreite ziehen, sonst klebt alles links
                cv.itemconfigure(wid, width=event.width)
                cv.configure(scrollregion=cv.bbox("all"))

            def _on_wheel(event, cv=canvas):
                cv.yview_scroll(int(-event.delta / 120), "units")

            scrollable_frame.bind("<Configure>", _on_content)
            canvas.bind("<Configure>", _on_canvas)
            # Mausrad gilt nur, solange der Zeiger über diesem Tab steht.
            # `fn=_on_wheel` ist Pflicht, sonst zeigt der Name nach der Schleife
            # auf die zuletzt erzeugte Funktion -- dieselbe Falle wie oben.
            canvas.bind("<Enter>",
                        lambda e, cv=canvas, fn=_on_wheel: cv.bind_all("<MouseWheel>", fn))
            canvas.bind("<Leave>", lambda e, cv=canvas: cv.unbind_all("<MouseWheel>"))

            # Create fields for each register in this category
            for addr, info in regs.items():
                self.register_fields[addr] = tk.StringVar(value="")

                # Parse register info (support old string format and new dict format)
                if isinstance(info, dict):
                    reg_name = info.get("name", "")
                    reg_desc = info.get("description", "")
                    bitfields = info.get("bitfields", {})
                else:
                    reg_name = ""
                    reg_desc = str(info)
                    bitfields = {}

                self.register_meta[addr] = {
                    "category": category,
                    "name": reg_name,
                    "description": reg_desc,
                    "bitfields": bitfields,
                }

                # Main row (address, name, value, buttons)
                row = ttk.Frame(scrollable_frame)
                row.pack(fill=tk.X, padx=5, pady=1)

                ttk.Label(row, text=f"{addr}", width=14, font=("Courier", 9)).pack(side=tk.LEFT)
                name_text = f"{reg_name}" if reg_name else reg_desc[:20]
                ttk.Label(row, text=name_text, width=30).pack(side=tk.LEFT)

                value_var = self.register_fields[addr]
                value_entry = ttk.Entry(row, textvariable=value_var, width=14, font=("Courier", 9))
                value_entry.pack(side=tk.LEFT, padx=5)

                ttk.Button(row, text="Read", width=6, command=lambda a=addr: self.read_register(a)).pack(side=tk.LEFT, padx=2)
                ttk.Button(row, text="Write", width=6, command=lambda a=addr: self.write_register(a)).pack(side=tk.LEFT, padx=2)

                # Bitfields row (if available)
                if bitfields:
                    # Separator row
                    sep_row = ttk.Frame(scrollable_frame)
                    sep_row.pack(fill=tk.X, padx=20, pady=0)

                    # Description + Bitfields
                    desc_text = ttk.Label(sep_row, text=f"📋 {reg_desc}", font=("Courier", 8), foreground="#666")
                    desc_text.pack(anchor=tk.W)

                    # Bitfield definitions
                    for bits, meaning in bitfields.items():
                        bitfield_label = ttk.Label(sep_row, text=f"   [{bits}] {meaning}", font=("Courier", 8), foreground="#444")
                        bitfield_label.pack(anchor=tk.W)

                    # Decoded value label (updated when value changes)
                    decoded_var = tk.StringVar()
                    decoded_label = ttk.Label(sep_row, textvariable=decoded_var, font=("Courier", 8), foreground="#009900", wraplength=400)
                    decoded_label.pack(anchor=tk.W, padx=25)

                    # Create update function with proper closure
                    def make_update_decoded(val_var, bf_dict, dec_var):
                        def update_decoded(*args):
                            try:
                                hex_val = val_var.get()
                                if hex_val and hex_val.strip():
                                    decoded = self.decode_bitfields(hex_val, bf_dict)
                                    dec_var.set(f"✓ {decoded}")
                            except Exception as e:
                                dec_var.set(f"Error: {e}")
                        return update_decoded

                    callback = make_update_decoded(value_var, bitfields, decoded_var)
                    value_var.trace_add("write", callback)

            canvas.pack(side="left", fill="both", expand=True)
            scrollbar.pack(side="right", fill="y")

        # The bulk buttons that used to sit here are in the top bar now (create_widgets),
        # where they stay visible no matter which tab is open or how far it is scrolled.

    def create_testmodes_tab(self):
        """Create Test Modes tab"""
        frame = ttk.Frame(self.notebook)
        self.notebook.add(frame, text="Test Modes")

        # Test mode selection
        info_frame = ttk.LabelFrame(frame, text="IEEE 802.3 Test Modes (0x000308FB)", padding=10)
        info_frame.pack(fill=tk.X, padx=5, pady=5)

        ttk.Label(
            info_frame,
            text="Select a test mode and optionally set auto-revert timeout (seconds).\n"
                 "Mode 0 = Normal, 1-4 = Test modes, see LAN8651_TEST_MODES.md\n\n"
                 "⚠️  Warning: Test modes disconnect the T1S link. The bridge is unreachable during test.",
            justify=tk.LEFT,
            foreground="red"
        ).pack(anchor=tk.W)

        # Mode and timeout input
        input_frame = ttk.Frame(info_frame)
        input_frame.pack(fill=tk.X, pady=10)

        ttk.Label(input_frame, text="Mode:").pack(side=tk.LEFT, padx=5)
        self.testmode_var = tk.StringVar(value="0")
        mode_spin = ttk.Spinbox(
            input_frame,
            from_=0, to=4,
            textvariable=self.testmode_var,
            width=5
        )
        mode_spin.pack(side=tk.LEFT, padx=5)

        ttk.Label(input_frame, text="Auto-revert (sec):").pack(side=tk.LEFT, padx=5)
        self.testmode_timeout_var = tk.StringVar(value="")
        ttk.Entry(input_frame, textvariable=self.testmode_timeout_var, width=8).pack(side=tk.LEFT, padx=5)

        # Buttons
        btn_frame = ttk.Frame(info_frame)
        btn_frame.pack(fill=tk.X, pady=10)

        ttk.Button(btn_frame, text="Apply Test Mode", command=self.apply_testmode, width=20).pack(side=tk.LEFT, padx=2)
        ttk.Button(btn_frame, text="Read Current Mode", command=self.read_testmode, width=20).pack(side=tk.LEFT, padx=2)

        # Hinweis auf die Testsuite. Bewusst kein Knopf: test_lan8651.py öffnet
        # den COM-Port selbst und kollidiert mit der offenen Verbindung dieser
        # GUI. Erst Disconnect, dann das Skript in einer Konsole starten.
        script_frame = ttk.LabelFrame(frame, text="Automated Testing", padding=10)
        script_frame.pack(fill=tk.X, padx=5, pady=5)

        ttk.Label(
            script_frame,
            text="Vollständige Testsuite (Readback, Verkehr stoppt, Verkehr kommt wieder):\n"
                 "    python test_lan8651.py --port <COM>\n"
                 "Vorher hier auf Disconnect drücken - das Skript braucht den Port exklusiv.",
            justify=tk.LEFT, font=("Courier", 9)
        ).pack(anchor=tk.W)

        # Info frame
        info_frame2 = ttk.LabelFrame(frame, text="Test Mode Reference", padding=10)
        info_frame2.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        ref_text = tk.Text(info_frame2, height=12, width=80, wrap=tk.WORD, state=tk.NORMAL)
        ref_text.pack(fill=tk.BOTH, expand=True)

        ref_content = """Mode 1 - Test Mode 1: Output Voltage & Timing Jitter
  Use: Oscilloscope
  Measures: Signal shape, timing jitter, rise/fall time

Mode 2 - Test Mode 2: Output Droop
  Use: Oscilloscope
  Measures: Voltage droop under load

Mode 3 - Test Mode 3: PSD Mask (Spectral Emissions)
  Use: Spectrum Analyzer
  Measures: Power spectral density, EMI compliance

Mode 4 - Test Mode 4: Transmitter High Impedance
  Use: Bus analyzer, probe without active sender
  Measures: Bus impedance, passive monitoring

For detailed measurement setup and verification procedures, see LAN8651_TEST_MODES.md"""

        ref_text.insert(1.0, ref_content)
        ref_text.config(state=tk.DISABLED)

    def create_terminal_tab(self):
        """Create Serial Terminal tab with gui_term.py logic"""
        frame = ttk.Frame(self.notebook)
        self.notebook.add(frame, text="Terminal")

        self.terminal_q = queue.Queue()
        self.terminal_link: Optional[Link] = None
        self.terminal_screen = Screen()
        self._terminal_seen_total = 0
        self._terminal_widget_lines = 0
        self._terminal_cursor_on = True

        # Clear button frame
        ctrl_frame = ttk.Frame(frame)
        ctrl_frame.pack(side=tk.TOP, fill=tk.X, padx=4, pady=4)

        ttk.Button(ctrl_frame, text="Alles löschen", width=14, command=self.terminal_clear_all).pack(side=tk.LEFT, padx=2)

        # Terminal display
        self.terminal_text = scrolledtext.ScrolledText(
            frame,
            wrap="char",
            width=100,
            height=30,
            font=("Consolas", 10),
            background="#101010",
            foreground="#d8d8d8",
            insertwidth=0
        )
        self.terminal_text.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=4, pady=4)
        self.terminal_text.tag_config("cursor", background="#d8d8d8", foreground="#101010")

        self.terminal_text.bind("<Key>", self.terminal_on_key)
        self.terminal_text.bind("<Control-c>", self.terminal_on_ctrl_c)
        self.terminal_text.bind("<Control-v>", self.terminal_on_paste)
        self.terminal_text.bind("<<Paste>>", self.terminal_on_paste)
        self.terminal_text.bind("<Button-3>", self.terminal_on_paste)
        self.terminal_text.bind("<Button-1>", lambda _e: self.terminal_text.focus_set())
        self.terminal_text.bind("<Prior>", lambda e: (self.terminal_text.yview_scroll(-1, "pages"), "break"))
        self.terminal_text.bind("<Next>", lambda e: (self.terminal_text.yview_scroll(1, "pages"), "break"))

        # Set focus to terminal
        self.terminal_text.focus_set()


    def terminal_disconnect(self):
        """Disconnect from terminal"""
        if self.terminal_link:
            self.terminal_link.close()
            self.terminal_link = None
            self.terminal_note("getrennt")

    def terminal_clear_all(self):
        """Clear terminal display"""
        self.terminal_screen = Screen()
        self._terminal_seen_total = 0
        self._terminal_widget_lines = 0
        self.terminal_text.delete(1.0, tk.END)

    def terminal_on_key(self, event):
        """Handle key input"""
        if event.keysym in ("Prior", "Next"):
            return  # Handled by bind
        if event.keysym in IGNORED_KEYSYMS:
            return "break"

        data = KEYSYM_BYTES.get(event.keysym)
        if data is None:
            if event.char:
                data = event.char.encode("latin-1", "ignore")
            elif (event.state & CONTROL_BIT) and len(event.keysym) == 1 and event.keysym.isalpha():
                data = bytes([ord(event.keysym.lower()) & 0x1F])

        if data:
            if self.port_link:
                try:
                    self.port_link.write(data)
                except OSError as e:
                    self.terminal_note(f"nicht verbunden: {e}")
            else:
                print("DEBUG: No port_link")
                self.terminal_note("nicht verbunden")

        return "break"

    def terminal_on_ctrl_c(self, _event=None):
        """Ctrl+C: copy or send interrupt"""
        try:
            selected = self.terminal_text.get("sel.first", "sel.last")
            if selected:
                self.terminal_text.clipboard_clear()
                self.terminal_text.clipboard_append(selected)
                return "break"
        except tk.TclError:
            pass

        if self.port_link:
            try:
                self.port_link.write(b"\x03")
            except OSError:
                self.terminal_note("nicht verbunden")
        return "break"

    def terminal_on_paste(self, _event=None):
        """Paste from clipboard"""
        if not self.port_link:
            self.terminal_note("nicht verbunden")
            return "break"
        try:
            text = self.root.clipboard_get()
            lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
            if lines and lines[-1] == "":
                lines.pop()
            if lines:
                self._terminal_paste_next(lines)
        except Exception:
            pass
        return "break"

    def _terminal_paste_next(self, lines):
        """Paste lines one by one with delay"""
        if not lines or not self.port_link:
            return
        try:
            self.port_link.write(lines[0].encode("latin-1", "ignore") + b"\r\n")
        except OSError:
            return
        if len(lines) > 1:
            self.root.after(120, self._terminal_paste_next, lines[1:])

    def terminal_note(self, line):
        """Add note to terminal"""
        data = f"\r\n[{line}]\r\n".encode("latin-1", "replace")
        self.terminal_screen.feed(data)
        self._terminal_render()

    def terminal_feed(self, data):
        """Feed bytes to terminal"""
        if not data:
            return
        self.terminal_screen.feed(data)
        self._terminal_render()

    def _terminal_render(self):
        """Render screen to text widget"""
        added = self.terminal_screen.total - self._terminal_seen_total
        new_lines = []
        if added > 0:
            new_lines = (self.terminal_screen.lines[-added:]
                         if added <= len(self.terminal_screen.lines)
                         else list(self.terminal_screen.lines))

        first = self._terminal_widget_lines + 1
        at_bottom = self.terminal_text.yview()[1] > 0.999

        # Delete old current line and insert new content
        self.terminal_text.delete(f"{first}.0", f"{first}.end")
        self.terminal_text.insert(f"{first}.0",
                                 "".join(line + "\n" for line in new_lines)
                                 + self.terminal_screen.cur + " ")
        self._terminal_widget_lines += len(new_lines)
        self._terminal_seen_total = self.terminal_screen.total

        if self._terminal_widget_lines > MAX_VIEW_LINES:
            drop = self._terminal_widget_lines - MAX_VIEW_LINES
            self.terminal_text.delete("1.0", f"{drop + 1}.0")
            self._terminal_widget_lines -= drop

        self._terminal_place_cursor()
        if at_bottom:
            self.terminal_text.see(tk.END)

    def _terminal_place_cursor(self):
        """Place/render cursor"""
        self.terminal_text.tag_remove("cursor", "1.0", tk.END)
        if not self._terminal_cursor_on:
            return
        row = self._terminal_widget_lines + 1
        col = self.terminal_screen.col
        self.terminal_text.tag_add("cursor", f"{row}.{col}", f"{row}.{col + 1}")

    def terminal_blink(self):
        """Blink cursor"""
        self._terminal_cursor_on = not self._terminal_cursor_on
        self._terminal_place_cursor()

    def terminal_process_queue(self):
        """Process messages from terminal queue"""
        try:
            while True:
                port, kind, payload = self.terminal_q.get_nowait()
                if kind == "data":
                    self.terminal_feed(payload)
                elif kind == "lost":
                    self.terminal_note(f"Verbindung verloren: {payload}")
                    self.port_link = None
                    self.disconnect_device()
        except queue.Empty:
            pass

    def create_about_tab(self):
        """Create About/Help tab"""
        frame = ttk.Frame(self.notebook)
        self.notebook.add(frame, text="Help")

        text = tk.Text(frame, wrap=tk.WORD, padx=10, pady=10)
        text.pack(fill=tk.BOTH, expand=True)

        help_text = """Bridge Status & Configuration GUI
Version 1.0

This GUI provides an interface to the T1S/100BASE-T Bridge firmware.

TABS:
- Bridge Parameters: Read/write bridge configuration (IP, MAC, PLCA)
- LAN8651 Registers: Direct register access with individual read/write
- Test Modes: Apply IEEE test modes and run diagnostic scripts
- Help: This page

FEATURES:
- Individual Read/Write: Each parameter/register has its own Read/Write button
- Bulk Operations: Read/Write all bridge parameters or registers at once
- JSON Persistence: Save configuration to file and reload it
- Threading: Long-running operations don't freeze the GUI
- Status Updates: Real-time feedback on each operation

WORKFLOW:
1. Select the correct COM port (e.g., COM8)
2. Use Read All to fetch current state from bridge
3. Edit values as needed
4. Use Write All to apply changes to bridge
5. Use Save to JSON to persist configuration

REGISTER ACCESS:
- Addresses use MMS encoding (upper 16 bits = bank, lower 16 bits = offset)
- Examples: 0x000308FB = Test Mode, 0x0004CA02 = PLCA_CTRL1
- Always read back after write to verify

TEST MODES:
- Modes 1-4 disconnect the link during the test
- Use testmode command for automatic readback and verification
- See LAN8651_TEST_MODES.md for measurement setup

CLI COMMAND:
python cli.py --port COM8 --read 1 "<command>"

Example commands:
  stats              - Show traffic statistics
  lan_read 0x0004CA02      - Read PLCA_CTRL1
  lan_write 0x0004CA02 0x80    - Write PLCA_CTRL1
  testmode 1         - Apply test mode 1
  mirror 1           - Enable port mirror
"""

        text.insert(1.0, help_text)
        text.config(state=tk.DISABLED)

    def refresh_com_ports(self):
        """Refresh the list of available COM ports"""
        available_ports = get_available_com_ports()

        if not available_ports:
            messagebox.showwarning("No COM Ports", "No COM ports detected. Check device connection.")
            self.comport_combo['values'] = []
            self.set_error_status("No COM ports available")
            return

        self.comport_combo['values'] = available_ports

        # If current selection is not in list, pick first available
        if self.comport_var.get() not in available_ports:
            self.comport_var.set(available_ports[0])

        port_list = ", ".join(available_ports)
        self.set_status(f"Found {len(available_ports)} port(s): {port_list}")

    def update_comport(self):
        """Update COM port in config"""
        port = self.comport_var.get()
        if not port:
            messagebox.showwarning("Warning", "Please select a COM port")
            return

        self.config["comport"] = port
        self.save_config()
        self.set_status(f"COM port set to {port}")

    def set_status(self, message: str, duration: int = 3000):
        """Update status label with message"""
        self.status_label.config(text=message, foreground="green")
        if duration > 0:
            self.root.after(duration, lambda: self.status_label.config(text="Ready", foreground="blue"))

    def set_error_status(self, message: str):
        """Update status label with error"""
        self.status_label.config(text=message, foreground="red")

    def clear_bridge_output(self):
        """Clear the bridge command output text widget"""
        self.bridge_output.config(state=tk.NORMAL)
        self.bridge_output.delete(1.0, tk.END)
        self.bridge_output.config(state=tk.DISABLED)

    def update_connection_indicator(self):
        """Update the connection indicator circle and label"""
        if self.connected:
            self.connection_indicator.delete("all")
            self.connection_indicator.create_oval(2, 2, 13, 13, fill="green", outline="darkgreen")
            self.connection_label.config(text="Online", foreground="green")
        else:
            self.connection_indicator.delete("all")
            self.connection_indicator.create_oval(2, 2, 13, 13, fill="red", outline="darkred")
            self.connection_label.config(text="Offline", foreground="red")

    def connect_device(self):
        """Open COM port (shared by CLI + Terminal)"""
        port = self.comport_var.get()
        if not port:
            messagebox.showwarning("Warning", "Please select a COM port first")
            return

        if self.port_link:
            messagebox.showinfo("Info", "Already connected")
            return

        def worker():
            link = Link(port, self.result_queue)
            try:
                link.open()
                self.result_queue.put(("port_opened", link))
            except Exception as e:
                self.result_queue.put(("port_failed", str(e)))

        thread = threading.Thread(target=worker, daemon=True)
        thread.start()
        self.set_status("Connecting...")

    def disconnect_device(self):
        """Close COM port (for CLI + Terminal)"""
        if self.port_link:
            self.port_link.close()
            self.port_link = None
            # Terminal note
            if hasattr(self, 'terminal_note'):
                self.terminal_note("getrennt")

        self.connected = False
        self.update_connection_indicator()
        self.set_status("Disconnected")

    def run_async_cmd(self, command: str, timeout_ms: int = 1500):
        """Kommando über den offenen Link absetzen, Antwort ins Command Output."""
        if not self.port_link:
            self.set_error_status("Not connected")
            messagebox.showwarning("Nicht verbunden",
                                   "Erst auf Connect drücken, dann das Kommando.")
            return

        def worker():
            output = self.send_command_via_link(command, timeout_ms=timeout_ms)
            text = self.clean_response(command, output)
            self.result_queue.put(("cmd_result", bool(text), text or "keine Antwort"))

        threading.Thread(target=worker, daemon=True).start()
        self.set_status(f"Running: {command}")

    @staticmethod
    def clean_response(command: str, output: str) -> str:
        """Echo des Kommandos und Prompt-Zeichen aus der Antwort entfernen."""
        lines = []
        for raw in output.replace("\r", "\n").split("\n"):
            line = raw.strip().lstrip(">").strip()
            if not line or line == command.strip():
                continue
            lines.append(line)
        return "\n".join(lines)

    def _blink_loop(self):
        """Blink cursor in terminal"""
        if hasattr(self, 'terminal_blink'):
            self.terminal_blink()
        self.root.after(BLINK_MS, self._blink_loop)

    def process_queue(self):
        """Process results from background threads"""
        try:
            while True:
                result = self.result_queue.get_nowait()

                # Serielle Daten gehen an BEIDE Konsumenten: ans Terminal zur
                # Anzeige und - nur solange ein Kommando läuft - an den Worker.
                # Zwei Queues, je eine Kopie, deshalb kein Wettlauf um die Chunks.
                if len(result) >= 3 and result[1] == "data":
                    self.terminal_q.put(result)
                    if self.cmd_pending.is_set():
                        self.cmd_response_q.put(result)
                    continue

                if result[0] == "port_opened":
                    _, link = result
                    self.port_link = link
                    self.connected = True
                    self.update_connection_indicator()
                    self.set_status(f"Connected to {link.port}", duration=2000)
                    if hasattr(self, 'terminal_note'):
                        self.terminal_note(f"verbunden: {link.port}")
                    if hasattr(self, 'bridge_output'):
                        self.bridge_output.config(state=tk.NORMAL)
                        timestamp = time.strftime("%H:%M:%S")
                        self.bridge_output.insert(tk.END, f"[{timestamp}] ✓ Connected to {link.port}\n\n")
                        self.bridge_output.config(state=tk.DISABLED)

                elif result[0] == "port_failed":
                    _, error = result
                    self.connected = False
                    self.update_connection_indicator()
                    self.set_error_status(f"Connection failed: {error}")
                    messagebox.showerror("Connection Error", error)

                elif result[0] == "connect_result":
                    _, success, message = result
                    if success:
                        self.set_status("Command OK", duration=2000)
                    else:
                        self.set_error_status(f"Error: {message}")

                elif result[0] == "cmd_result":
                    _, success, output = result
                    if success:
                        self.set_status("Command OK", duration=2000)
                        # Show output in bridge_output text widget
                        if hasattr(self, 'bridge_output'):
                            self.bridge_output.config(state=tk.NORMAL)
                            timestamp = time.strftime("%H:%M:%S")
                            self.bridge_output.insert(tk.END, f"[{timestamp}] {output}\n\n")
                            self.bridge_output.see(tk.END)
                            self.bridge_output.config(state=tk.DISABLED)
                    else:
                        self.set_error_status(f"Error: {output}")
                        messagebox.showerror("Error", f"Command failed:\n{output}")

                elif result[0] == "register_read":
                    _, addr, success, value = result
                    if success and value:
                        self.register_fields[addr].set(value)
                        self.set_status(f"Read {addr}: {value}", duration=2000)
                    else:
                        self.set_error_status(f"Failed to read {addr}")

                elif result[0] == "bulk_progress":
                    _, done, total, failed = result
                    self.set_status(f"Reading registers {done}/{total}"
                                    + (f"  ({failed} ohne Antwort)" if failed else ""))

                elif result[0] == "bridge_read":
                    _, key, success, value = result
                    if success:
                        self.bridge_fields[key].set(value)
                        self.set_status(f"Read {key}: {value}", duration=2000)
                    else:
                        self.set_error_status(f"Failed to read {key}")

        except queue.Empty:
            pass

        # Process terminal queue
        if hasattr(self, 'terminal_process_queue'):
            self.terminal_process_queue()

        self.root.after(POLL_MS, self.process_queue)

    # Register read/write via open Link (not cli.py)
    def decode_bitfields(self, value_hex: str, bitfields: dict) -> str:
        """Decode bitfield value into human-readable format with interpretations

        Args:
            value_hex: Hex value like "0x00000800"
            bitfields: Dict like {"15:8": "NODE_CNT — Number of nodes...", "7:0": "NODE_ID — ..."}

        Returns:
            Formatted string with extracted and interpreted values
        """
        if not value_hex or not bitfields:
            return ""

        try:
            value = int(value_hex, 16)
        except (ValueError, TypeError):
            return ""

        decoded = []
        for bits_range, meaning in bitfields.items():
            # Feldname ist der Teil vor dem Trennstrich, so wie ihn die Config
            # aus dem Datenblatt mitbringt ("NODE_CNT - PLCA node count").
            key = meaning.split(" - ")[0].strip()

            try:
                if ":" in bits_range:
                    high, low = map(int, bits_range.split(":"))
                    width = high - low + 1
                    field_value = (value >> low) & ((1 << width) - 1)
                else:
                    width = 1
                    field_value = (value >> int(bits_range)) & 1
            except ValueError:
                continue

            # Keine Interpretation raten: bei einem Bit 0/1, sonst dezimal und
            # hex. Was die Werte bedeuten, steht in der Zeile darüber.
            if width == 1:
                decoded.append(f"{key}={field_value}")
            else:
                decoded.append(f"{key}={field_value} (0x{field_value:X})")

        return "  ".join(decoded)

    def send_command_via_link(self, cmd: str, timeout_ms: int = 700) -> str:
        """Kommando über den offenen Link schicken und auf die Antwort warten.

        Läuft im Worker-Thread. Die Antwort kommt über cmd_response_q, die nur
        gefüllt wird, solange cmd_pending gesetzt ist -- damit liest der Terminal-
        Konsument im Main-Thread nicht dieselben Chunks weg.
        """
        if not self.port_link:
            return "ERROR: Not connected"

        # Nur ein Kommando gleichzeitig, sonst mischen sich zwei Antworten.
        with self.cmd_lock:
            # Altbestand verwerfen, sonst landet die Antwort des Vorgängers hier.
            while True:
                try:
                    self.cmd_response_q.get_nowait()
                except queue.Empty:
                    break

            self.cmd_pending.set()
            try:
                self.port_link.write(cmd.encode() + b"\r")

                start = time.time()
                chunks = []
                idle = 0

                while time.time() - start < timeout_ms / 1000.0:
                    try:
                        port, kind, payload = self.cmd_response_q.get(timeout=0.01)
                    except queue.Empty:
                        # Erst abbrechen, wenn schon etwas da war -- sonst wartet
                        # der erste Durchlauf gar nicht auf das Gerät.
                        idle += 1
                        if chunks and idle > 4:
                            break
                        continue

                    if kind != "data":
                        continue
                    chunks.append(payload.decode("latin-1", "ignore"))
                    idle = 0
                    text = "".join(chunks)
                    # Fertig, sobald der Marker da ist UND die Zeile abgeschlossen
                    # wurde -- ein "OK:" ohne Zeilenende ist erst der Anfang.
                    for marker in ("OK:", "ERROR"):
                        pos = text.find(marker)
                        if pos >= 0 and "\n" in text[pos:]:
                            return text

                return "".join(chunks)
            finally:
                self.cmd_pending.clear()

    # Bridge parameter methods
    def read_all_bridge(self):
        """Alle Bridge-Parameter mit einem showenv holen."""
        if not self.port_link:
            self.set_error_status("Not connected")
            messagebox.showwarning("Nicht verbunden", "Erst auf Connect drücken.")
            return

        self.set_status("Reading bridge parameters...")

        def worker():
            output = self.send_command_via_link("showenv", timeout_ms=1500)
            found = 0
            for key in self.bridge_fields:
                value = self.parse_showenv(output, key)
                if value is not None:
                    found += 1
                self.result_queue.put(("bridge_read", key, value is not None, value or ""))
            self.result_queue.put(("cmd_result", found > 0,
                                   self.clean_response("showenv", output)
                                   or "showenv: keine Antwort"))

        threading.Thread(target=worker, daemon=True).start()

    def write_all_bridge(self):
        """Alle geänderten Bridge-Parameter schreiben.

        setenv schreibt nur die RAM-Kopie; erst saveenv legt sie ins EEPROM.
        Deshalb wird am Ende gefragt, ob persistiert werden soll.
        """
        if not self.port_link:
            self.set_error_status("Not connected")
            messagebox.showwarning("Nicht verbunden", "Erst auf Connect drücken.")
            return

        cmds = []
        for key, var in self.bridge_fields.items():
            value = var.get().strip()
            if not value:
                continue
            cmd = self.bridge_write_command(key, value)
            if cmd:
                cmds.append(cmd)

        if not cmds:
            messagebox.showinfo("Info", "Keine schreibbaren Parameter gefüllt.")
            return

        if not messagebox.askyesno("Bestätigen",
                                   "Folgende Kommandos an das Gerät schicken?\n\n"
                                   + "\n".join(cmds)):
            return

        persist = messagebox.askyesno(
            "Persistieren?",
            "Anschließend 'saveenv' ausführen?\n\n"
            "Ja  = dauerhaft ins EEPROM (übersteht Reset)\n"
            "Nein = nur RAM, bis zum nächsten Reset")

        def worker():
            log = []
            for cmd in cmds:
                out = self.send_command_via_link(cmd, timeout_ms=1500)
                log.append(f"> {cmd}\n{self.clean_response(cmd, out)}")
            if persist:
                out = self.send_command_via_link("saveenv", timeout_ms=3000)
                log.append(f"> saveenv\n{self.clean_response('saveenv', out)}")
            self.result_queue.put(("cmd_result", True, "\n".join(log)))

        threading.Thread(target=worker, daemon=True).start()
        self.set_status("Writing bridge parameters...")

    @staticmethod
    def bridge_write_command(key: str, value: str) -> Optional[str]:
        """Feldname -> setenv-Kommando (Schlüssel wie in env.c:cmd_setenv)."""
        mapping = {
            "ip_eth0":  f"setenv ip0 {value}",
            "ip_eth1":  f"setenv ip1 {value}",
            "mac_eth0": f"setenv mac0 {value}",
            "mac_eth1": f"setenv mac1 {value}",
            "plca_id":  f"setenv plca_id {value}",
            "plca_cnt": f"setenv plca_cnt {value}",
        }
        return mapping.get(key)

    @staticmethod
    def parse_showenv(output: str, key: str) -> Optional[str]:
        """Einen Wert aus der showenv-Ausgabe ziehen (Format: env.c:261-286).

          eth0  ip 192.168.0.200  mask 255.255.255.0  gw ...  dns ...
          eth0  mac 00:04:25:1A:00:00
          plca  id 0  count 8  (eth0/T1S)
        """
        if key in ("ip_eth0", "ip_eth1", "mac_eth0", "mac_eth1"):
            what, iface = key.split("_")          # "ip"/"mac", "eth0"/"eth1"
            for line in output.splitlines():
                line = line.strip()
                if not line.startswith(iface):
                    continue
                m = re.search(rf'\b{what}\s+(\S+)', line)
                if m:
                    return m.group(1)
            return None

        if key in ("plca_id", "plca_cnt"):
            m = re.search(r'plca\s+id\s+(\d+)\s+count\s+(\d+)', output)
            if m:
                return m.group(1) if key == "plca_id" else m.group(2)
        return None

    def read_bridge_field(self, key: str):
        """Einen Bridge-Parameter über den offenen Link lesen."""
        if not self.port_link:
            self.set_error_status("Not connected")
            return

        def worker():
            output = self.send_command_via_link("showenv", timeout_ms=1500)
            value = self.parse_showenv(output, key)
            self.result_queue.put(("bridge_read", key, value is not None, value or ""))

        threading.Thread(target=worker, daemon=True).start()

    def write_bridge_field(self, key: str):
        """Write a specific bridge field"""
        value = self.bridge_fields.get(key, tk.StringVar()).get()

        if not value:
            messagebox.showwarning("Warning", f"No value for {key}")
            return

        # Map fields to commands
        commands = {
            "plca_id": f"plca_node {value}",
            "plca_cnt": f"setenv plca_cnt {value}",
        }

        cmd = commands.get(key)
        if cmd:
            self.run_async_cmd(cmd)
        else:
            messagebox.showinfo("Info", f"No write command for {key}")

    def save_bridge_json(self):
        """Save bridge parameters to JSON"""
        self.config["bridge"] = {}
        for key, var in self.bridge_fields.items():
            value = var.get()
            try:
                # Try to convert to number if possible
                if '.' in value:
                    self.config["bridge"][key] = float(value)
                else:
                    self.config["bridge"][key] = int(value) if value.isdigit() else value
            except ValueError:
                self.config["bridge"][key] = value

        self.save_config()
        self.set_status("Bridge config saved to JSON")

    def load_bridge_json(self):
        """Load bridge parameters from JSON"""
        if not CONFIG_FILE.exists():
            messagebox.showwarning("Warning", "Config file not found")
            return

        cfg = json.load(open(CONFIG_FILE, encoding="utf-8"))
        bridge_cfg = cfg.get("bridge", {})

        for key, value in bridge_cfg.items():
            if key in self.bridge_fields:
                self.bridge_fields[key].set(str(value))

        self.set_status("Bridge config loaded from JSON")

    # Register methods
    def read_register(self, addr: str):
        """Read a single register via open Link"""
        def worker():
            if not self.port_link:
                self.result_queue.put(("register_read", addr, False, "Not connected"))
                return

            output = self.send_command_via_link(f"lan_read {addr}")
            value = self.cli.parse_register_read(output)
            if not value:
                value = output.strip()

            success = "OK:" in output
            self.result_queue.put(("register_read", addr, success, value))

        thread = threading.Thread(target=worker, daemon=True)
        thread.start()

    def write_register(self, addr: str):
        """Write a single register via open Link"""
        value = self.register_fields.get(addr, tk.StringVar()).get()

        if not value:
            messagebox.showwarning("Warning", f"No value for {addr}")
            return

        if not value.startswith("0x"):
            value = "0x" + value

        def worker():
            if not self.port_link:
                self.result_queue.put(("register_read", addr, False, "Not connected"))
                return

            # Write
            output = self.send_command_via_link(f"lan_write {addr} {value}")
            time.sleep(0.2)

            # Read back to verify
            output_rb = self.send_command_via_link(f"lan_read {addr}")
            value_readback = self.cli.parse_register_read(output_rb)
            success_rb = "OK:" in output_rb

            self.result_queue.put(("register_read", addr, success_rb, value_readback or output_rb.strip()))

        thread = threading.Thread(target=worker, daemon=True)
        thread.start()
        self.set_status(f"Writing {addr} = {value}...")

    def bulk_read_registers(self):
        """Alle Register des aktuellen Tabs lesen."""
        addrs = list(self.register_fields.keys())
        self.set_status(f"Reading {len(addrs)} registers...")

        def worker():
            if not self.port_link:
                self.set_error_status("Not connected")
                return

            failed = []
            for n, addr in enumerate(addrs, 1):
                output = self.send_command_via_link(f"lan_read {addr}")
                value = self.cli.parse_register_read(output)
                if not value:
                    m = re.search(r'Value=(0x[0-9A-Fa-f]+)', output)
                    value = m.group(1) if m else ""

                if value:
                    self.result_queue.put(("register_read", addr, True, value))
                else:
                    failed.append(addr)
                    self.result_queue.put(("register_read", addr, False, ""))

                if n % 10 == 0 or n == len(addrs):
                    self.result_queue.put(("bulk_progress", n, len(addrs), len(failed)))

            if failed:
                self.result_queue.put(("cmd_result", True,
                                       f"Bulk read: {len(addrs)-len(failed)}/{len(addrs)} ok, "
                                       f"keine Antwort von: {', '.join(failed[:12])}"
                                       + (" ..." if len(failed) > 12 else "")))

        threading.Thread(target=worker, daemon=True).start()

    def bulk_write_registers(self):
        """Write all registers via open Link"""
        if not messagebox.askyesno("Confirm", "Write all registers to device?"):
            return

        self.set_status("Writing all registers...")

        def worker():
            if not self.port_link:
                self.set_error_status("Not connected")
                return

            for addr, var in self.register_fields.items():
                value = var.get()
                if not value:
                    continue

                if not value.startswith("0x"):
                    value = "0x" + value

                self.send_command_via_link(f"lan_write {addr} {value}")
                time.sleep(0.15)

            # Bulk read to verify
            self.result_queue.put(("cmd_result", True, "All registers written. Reading back..."))
            time.sleep(0.2)

            for addr in self.register_fields.keys():
                output = self.send_command_via_link(f"lan_read {addr}")
                value = self.cli.parse_register_read(output)
                success = "OK:" in output
                if value:
                    self.result_queue.put(("register_read", addr, success, value))

                time.sleep(0.15)

        thread = threading.Thread(target=worker, daemon=True)
        thread.start()

    def save_registers_json(self):
        """Werte speichern -- die Registerkarte dabei UNANGETASTET lassen.

        Diese Funktion hat die Karte früher zerstört: sie setzte
        self.config["registers"] = {} und schrieb dann {Adresse: Wert}. Nach dem
        ersten Klick auf 'Save to JSON' waren Mnemonic, Beschreibung und
        Bitfelder aller 182 Register weg, und die GUI zeigte danach nur noch
        nackte Adressen -- ohne Fehlermeldung. Deshalb wird hier ausschließlich
        das Feld 'value' fortgeschrieben, und die Struktur kommt aus
        register_meta, nicht aus dem, was gerade in der Datei steht.
        """
        if not self.register_meta:
            messagebox.showwarning("Warnung", "Keine Registerkarte geladen - nichts gespeichert.")
            return

        registers: Dict[str, dict] = {}
        for addr, meta in self.register_meta.items():
            category = meta["category"]
            registers.setdefault(category, {})[addr] = {
                "name": meta["name"],
                "description": meta["description"],
                "value": self.register_fields[addr].get(),
                "bitfields": meta["bitfields"],
            }

        self.config["registers"] = registers
        self.save_config()

        n_val = sum(1 for a in self.register_meta if self.register_fields[a].get())
        self.set_status(f"{len(self.register_meta)} Register gespeichert "
                        f"({n_val} mit Wert)", duration=3000)

    def load_registers_json(self):
        """Load registers from JSON"""
        if not CONFIG_FILE.exists():
            messagebox.showwarning("Warning", "Config file not found")
            return

        cfg = json.load(open(CONFIG_FILE, encoding="utf-8"))
        regs = cfg.get("registers", {})

        n = 0
        for key, value in regs.items():
            if isinstance(value, dict):
                # Kategorie -> {Adresse: Eintrag}. Der Eintrag ist entweder das
                # volle Objekt aus der Datenblatt-Karte oder (alt) ein Wertstring.
                for addr, entry in value.items():
                    if addr not in self.register_fields:
                        continue
                    val = entry.get("value", "") if isinstance(entry, dict) else str(entry)
                    self.register_fields[addr].set(val)
                    n += 1 if val else 0
            elif key in self.register_fields:
                # Ganz altes flaches Format: Adresse -> Wert
                self.register_fields[key].set(str(value))
                n += 1

        self.set_status(f"{n} Registerwerte aus JSON geladen", duration=3000)

    # Test mode methods
    def read_testmode(self):
        """Read current test mode"""
        self.run_async_cmd("lan_read 0x000308FB")

    def apply_testmode(self):
        """Apply selected test mode"""
        mode = self.testmode_var.get()
        timeout = self.testmode_timeout_var.get()

        cmd = f"testmode {mode}"
        if timeout:
            cmd += f" {timeout}"

        self.run_async_cmd(cmd)


def main():
    root = tk.Tk()
    gui = BridgeGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
