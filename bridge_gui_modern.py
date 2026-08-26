#!/usr/bin/env python3
"""
Bridge Status & Configuration GUI - modern theme build

The same GUI as bridge_gui.py, restyled with sv-ttk (Sun Valley), a ttk theme
that mimics the Windows 11 Fluent look. bridge_gui.py itself is untouched -
BridgeGUIModern(BridgeGUI) adds nothing but the theme and a color restoration
pass for the semantic colors sv-ttk erases; every tab, every command, all
serial I/O and parsing are inherited unchanged.

    python bridge_gui_modern.py            dark (default)
    python bridge_gui_modern.py --light    light variant

Needs the extra "sv-ttk" package (not in bridge_gui.py's dependency list) -
see requirements.txt.

Two things about sv-ttk that were not obvious from its own docs, found by
testing rather than assumed - see FALLSTRICKE.md for the dated write-up:

1. It is far more invasive than "a ttk theme": applying it also recolors the
   root window and every PLAIN (non-ttk) widget - tk.Canvas, tk.Text - via
   Tk's option database, not just ttk widgets. No manual patch needed for
   those; they come out dark on their own.

2. It reapplies its palette via an idle task the first time the event loop
   turns, and that reapply STEAMROLLS every ttk.Label's construction-time
   `foreground=` - including the ones bridge_gui.py uses to carry real
   information: red errata/warning text, green decoded bitfield values, the
   red "AFTER RESET"/"NEXT BOOT" hints. There is no hook back to "what this
   label originally was" once that happens - color info, not just style, is
   gone. _restore_semantic_colors() reconstructs it from each label's TEXT
   content instead (the only thing still available), which is fragile in the
   sense that it silently stops matching if bridge_gui.py's wording ever
   changes - the trade-off for a thin subclass that does not duplicate the
   tab-building code. And it must run AFTER at least one root.update() /
   update_idletasks() - patching before that idle task has fired gets
   silently reverted by it right afterwards; verified by testing both orders,
   not assumed.
"""

import argparse
import ctypes
import tkinter as tk
from tkinter import ttk

import sv_ttk

import bridge_gui

# Chosen for legibility on sv-ttk's dark background (#1c1c1c); the light
# variant reuses bridge_gui.py's own original colors, which were already
# tuned for a light background.
RED_DARK = "#ff6b6b"
GREEN_DARK = "#4ac94a"
MUTED_DARK = "#9a9a9a"
RED_LIGHT = "#b00000"
GREEN_LIGHT = "#009900"
MUTED_LIGHT = "#555555"


class BridgeGUIModern(bridge_gui.BridgeGUI):
    """sv-ttk build of BridgeGUI - see the module docstring."""

    def __init__(self, root, dark: bool = True):
        self._dark = dark
        # Before super().__init__() (which builds the top bar's ~8 buttons via the
        # inherited setup_ui()), not after: a style-level ttk.Style().configure()
        # applies to every button built afterwards, so this avoids a resize/flash
        # and - unlike the per-label color patch below - survives being set this
        # early; verified, see the module docstring and FALLSTRICKE.md.
        self._tighten_button_style()
        super().__init__(root)
        self.root.title("Bridge Status & Configuration (Modern)")
        # Forces sv-ttk's own idle-task restyle to run NOW, before we try to fix
        # anything it just broke - see point 2 in the module docstring.
        self.root.update_idletasks()
        self._restore_semantic_colors()
        self.update_connection_indicator()  # re-assert red/green now that it will stick
        # AFTER the window has its final size (BridgeGUI.__init__ already called
        # root.state("zoomed")), not before: the DWM attribute alone was set correctly
        # (return code 0 = S_OK) but the title bar visibly stayed light - a forced
        # non-client repaint is needed too, and there was no stable window to repaint
        # yet if this ran at the top of __init__ the way the first attempt did.
        self._apply_dark_titlebar(dark)

    def _apply_dark_titlebar(self, dark: bool) -> None:
        """Color the native Windows title bar to match - sv-ttk (and ttk in
        general) only reaches ttk/tk widgets, the title bar is the OS's own
        window chrome and has no tkinter API at all. Windows 10 (2004+) and
        Windows 11 expose it through the DWM, called here directly via ctypes.

        root.winfo_id() is the embedded CHILD window Tk hands out, not the real
        top-level HWND the title bar belongs to - GetParent() walks up to the
        one DWM actually needs; skipping that step is why naive versions of
        this recipe silently do nothing. Attribute 20 is
        DWMWA_USE_IMMERSIVE_DARK_MODE on Windows 11 and Windows 10 20H1+; 19 was
        the same attribute's number on the two Windows 10 builds just before
        that, tried as a fallback.

        Setting the attribute is not enough by itself - confirmed first-hand:
        DwmSetWindowAttribute returned 0 (S_OK) yet the title bar stayed light.
        DWM only repaints the non-client area (the title bar) on its own
        schedule; SetWindowPos with SWP_FRAMECHANGED forces that repaint now
        instead of waiting for one to happen to occur on its own (a resize, a
        focus change, ...).
        """
        try:
            hwnd = ctypes.windll.user32.GetParent(self.root.winfo_id())
            value = ctypes.c_int(1 if dark else 0)
            for attribute in (20, 19):
                ctypes.windll.dwmapi.DwmSetWindowAttribute(
                    hwnd, attribute, ctypes.byref(value), ctypes.sizeof(value))
            SWP_NOMOVE, SWP_NOSIZE, SWP_NOZORDER, SWP_FRAMECHANGED = 0x2, 0x1, 0x4, 0x20
            ctypes.windll.user32.SetWindowPos(
                hwnd, None, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED)
        except OSError:
            pass  # Windows build without this DWM attribute - title bar stays light

    @staticmethod
    def _tighten_button_style() -> None:
        """sv-ttk's own TButton padding ({8 2 8 3}, see theme/dark.tcl) plus its
        SunValleyBodyFont (~14px, noticeably bigger than the default ttk theme's
        font) make each button meaningfully wider than bridge_gui.py's top bar was
        ever sized for - measured: the ~8-button top bar grows from ~1250px to
        ~1580px required width. That is enough to push the connection indicator
        and status label (packed on the right) off the edge of a window that
        isn't extra wide, which is what was reported. Tightened back down; still
        legible, no longer silently clips the status area."""
        style = ttk.Style()
        style.configure("TButton", padding=(4, 2), font=("Segoe UI", 9))

    def _restore_semantic_colors(self) -> None:
        """Reconstruct bridge_gui.py's warning/success colors from each
        label's text - see the module docstring for why this is necessary
        and why it works this way."""
        red = RED_DARK if self._dark else RED_LIGHT
        green = GREEN_DARK if self._dark else GREEN_LIGHT
        muted = MUTED_DARK if self._dark else MUTED_LIGHT

        env_widget = getattr(self, "_env_identity_widget", None)
        if env_widget is not None:
            env_widget.configure(foreground=muted)

        def walk(widget):
            labels = [c for c in widget.winfo_children() if isinstance(c, ttk.Label)]
            for i, label in enumerate(labels):
                text = str(label.cget("text")).strip()
                if text.startswith("⚠") or text.startswith("->"):
                    # "⚠ <errata items>" (register row) and the errata summary/
                    # implication lines in the bitfield section - all warning-red.
                    label.configure(foreground=red)
                elif text in ("AFTER RESET", "NEXT BOOT"):
                    # The "applies" hint next to mac0/mac1/mirror in Bridge Parameters.
                    label.configure(foreground=red)
                elif text.startswith("\U0001f4cb"):
                    # The bitfield section's register-description line.
                    label.configure(foreground=muted)
                elif text.startswith("["):
                    # "[bits] meaning" - the label right after it in the same row is
                    # the decoded VALUE (a StringVar, usually empty here, so it can't
                    # be matched by its own text) - identified structurally instead.
                    label.configure(foreground=muted)
                    if i + 1 < len(labels):
                        labels[i + 1].configure(foreground=green)
                elif text.startswith("Model:") or text == "no register model loaded":
                    label.configure(foreground=muted)
            for child in widget.winfo_children():
                walk(child)

        walk(self.root)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--light", action="store_true", help="use the light variant instead of dark")
    args = ap.parse_args()

    root = tk.Tk()
    sv_ttk.set_theme("light" if args.light else "dark")
    BridgeGUIModern(root, dark=not args.light)
    root.mainloop()


if __name__ == "__main__":
    main()
